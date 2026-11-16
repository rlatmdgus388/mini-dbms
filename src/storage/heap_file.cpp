#include "storage/heap_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

namespace minidb {

namespace {
Status io_error(const std::string& what) {
  return Status::fail(ErrorCode::kIo, what + ": " + std::strerror(errno));
}

// pread를 끝까지 반복. 읽은 바이트 수 또는 -1.
ssize_t pread_full(int fd, char* buf, size_t n, uint64_t off) {
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd, buf + done, n - done, static_cast<off_t>(off + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) break;
    done += static_cast<size_t>(r);
  }
  return static_cast<ssize_t>(done);
}
}  // namespace

Result<std::unique_ptr<HeapFile>> HeapFile::open(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return io_error("cannot open " + path);
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    Status s = io_error("cannot stat " + path);
    ::close(fd);
    return s;
  }
  return std::unique_ptr<HeapFile>(new HeapFile(fd, static_cast<uint64_t>(st.st_size)));
}

HeapFile::~HeapFile() {
  if (fd_ >= 0) ::close(fd_);
}

void HeapFile::frame_record(const std::string& payload, std::string* out) {
  uint32_t n = static_cast<uint32_t>(payload.size());
  out->append(reinterpret_cast<const char*>(&n), sizeof(n));
  out->append(payload);
}

Status HeapFile::append(const std::string& framed) {
  size_t done = 0;
  while (done < framed.size()) {
    ssize_t w = ::pwrite(fd_, framed.data() + done, framed.size() - done, static_cast<off_t>(size_ + done));
    if (w < 0) {
      if (errno == EINTR) continue;
      Status s = io_error("write failed");
      if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) { /* best effort rollback */ }
      return s;
    }
    done += static_cast<size_t>(w);
  }
  size_ += framed.size();
  return Status::success();
}

Result<std::string> HeapFile::read_record(uint64_t offset) const {
  uint32_t n = 0;
  if (offset + sizeof(n) > size_ || pread_full(fd_, reinterpret_cast<char*>(&n), sizeof(n), offset) != sizeof(n))
    return Status::fail(ErrorCode::kIo, "record offset out of range");
  if (n > kMaxRecordSize || offset + sizeof(n) + n > size_) return Status::fail(ErrorCode::kIo, "corrupt record header");
  std::string payload(n, '\0');
  if (n > 0 && pread_full(fd_, payload.data(), n, offset + sizeof(n)) != static_cast<ssize_t>(n))
    return io_error("read failed");
  return payload;
}

Result<uint64_t> HeapFile::scan(const ScanFn& cb) const {
  constexpr size_t kChunk = 4u * 1024 * 1024;
  const uint64_t file_size = size_;
  std::vector<char> buf(kChunk);
  size_t have = 0;         // buf에 채워진 바이트 수
  uint64_t buf_start = 0;  // buf[0]의 파일 offset
  uint64_t file_pos = 0;   // 다음에 읽을 파일 offset
  size_t cursor = 0;       // buf 내 파싱 위치

  while (true) {
    size_t avail = have - cursor;
    uint32_t n = 0;
    bool need_more = avail < sizeof(n);
    if (!need_more) {
      std::memcpy(&n, buf.data() + cursor, sizeof(n));
      if (n > kMaxRecordSize)
        return Status::fail(ErrorCode::kIo, "corrupt record header at offset " + std::to_string(buf_start + cursor));
      need_more = avail < sizeof(n) + n;
    }
    if (need_more) {
      if (file_pos >= file_size) return buf_start + cursor;  // EOF: 남은 바이트는 찢어진 꼬리
      // 남은 조각을 앞으로 당기고 더 읽는다.
      std::memmove(buf.data(), buf.data() + cursor, avail);
      buf_start += cursor;
      have = avail;
      cursor = 0;
      if (have >= sizeof(n) && buf.size() < sizeof(n) + static_cast<size_t>(n)) buf.resize(sizeof(n) + n);
      size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size() - have, file_size - file_pos));
      ssize_t r = pread_full(fd_, buf.data() + have, want, file_pos);
      if (r < 0) return io_error("read failed");
      if (r == 0) return buf_start;  // 파일이 예상보다 짧음: 여기까지가 유효
      have += static_cast<size_t>(r);
      file_pos += static_cast<uint64_t>(r);
      continue;
    }
    uint64_t offset = buf_start + cursor;
    if (!cb(offset, buf.data() + cursor + sizeof(n), n)) return offset + sizeof(n) + n;
    cursor += sizeof(n) + n;
  }
}

Status HeapFile::truncate_to(uint64_t size) {
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) return io_error("truncate failed");
  size_ = size;
  return Status::success();
}

}  // namespace minidb
