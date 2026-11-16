#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "common/result.h"

namespace minidb {

// append-only 힙 파일. 레코드 = [u32 payload 길이][payload]. 레코드의 위치는 파일 offset.
// 스레드 안전성은 제공하지 않는다: 호출자(Table)가 읽기=shared, 쓰기=exclusive 락으로 보호한다.
// 읽기 함수(read_record/scan)는 pread 기반이라 shared 락 아래에서 동시에 호출해도 안전하다.
class HeapFile {
 public:
  static constexpr uint32_t kMaxRecordSize = 16u * 1024 * 1024;

  static Result<std::unique_ptr<HeapFile>> open(const std::string& path);
  ~HeapFile();
  HeapFile(const HeapFile&) = delete;
  HeapFile& operator=(const HeapFile&) = delete;

  uint64_t size() const { return size_; }

  static void frame_record(const std::string& payload, std::string* out);

  // 이미 frame_record로 감싼 바이트들을 파일 끝에 기록한다. 실패 시 원래 크기로 되돌린다.
  Status append(const std::string& framed);
  Result<std::string> read_record(uint64_t offset) const;

  // 순차 스캔. cb(offset, payload, len)가 false를 반환하면 중단.
  // 끝에 불완전한(찢어진) 레코드가 있으면 거기서 멈추고 마지막 정상 레코드 끝 offset을 반환한다.
  using ScanFn = std::function<bool(uint64_t, const char*, uint32_t)>;
  Result<uint64_t> scan(const ScanFn& cb) const;

  Status truncate_to(uint64_t size);

 private:
  HeapFile(int fd, uint64_t size) : fd_(fd), size_(size) {}
  int fd_;
  uint64_t size_;
};

}  // namespace minidb
