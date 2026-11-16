#include <fcntl.h>
#include <unistd.h>

#include "storage/heap_file.h"
#include "storage/record.h"
#include "storage/schema.h"
#include "test_util.h"

using namespace minidb;
using testing::TempDir;

TEST(schema_valid) {
  auto s = parse_schema(testing::kUsersSchema, "users");
  CHECK(s.ok());
  CHECK_EQ(s.value().columns.size(), size_t{4});
  CHECK_EQ(s.value().column_index("age"), 2);
  CHECK_EQ(s.value().column_index("nope"), -1);
  CHECK_EQ(s.value().columns[1].max_len, 10u);
}

TEST(schema_invalid_cases) {
  CHECK_ERR(parse_schema("not json", "t").status(), "invalid schema");
  CHECK_ERR(parse_schema("[]", "t").status(), "object");
  CHECK_ERR(parse_schema(R"J({"columns":[]})J", "t").status(), "non-empty");
  CHECK_ERR(parse_schema(R"J({"columns":[{"name":"a","type":"INT"}]})J", "t").status(), "first column must be 'id BIGINT'");
  CHECK_ERR(parse_schema(R"J({"columns":[{"name":"id","type":"INT"}]})J", "t").status(), "first column must be 'id BIGINT'");
  CHECK_ERR(parse_schema(R"J({"columns":[{"name":"id","type":"BIGINT"},{"name":"id","type":"INT"}]})J", "t").status(), "duplicate");
  CHECK_ERR(parse_schema(R"J({"columns":[{"name":"id","type":"BIGINT"},{"name":"x","type":"TEXT"}]})J", "t").status(), "unsupported type");
  CHECK_ERR(parse_schema(R"J({"columns":[{"name":"id","type":"BIGINT"},{"name":"x","type":"VARCHAR(0)"}]})J", "t").status(), "VARCHAR length");
  CHECK_ERR(parse_schema(R"J({"columns":[{"name":"id","type":"BIGINT"},{"name":"b d","type":"INT"}]})J", "t").status(), "bad column name");
  CHECK_ERR(parse_schema(R"J({"table":"other","columns":[{"name":"id","type":"BIGINT"}]})J", "t").status(), "does not match");
}

TEST(identifier_validation_blocks_path_traversal) {
  CHECK(is_valid_identifier("users"));
  CHECK(is_valid_identifier("_t1"));
  CHECK(!is_valid_identifier(""));
  CHECK(!is_valid_identifier("../etc"));
  CHECK(!is_valid_identifier("a/b"));
  CHECK(!is_valid_identifier("1abc"));
  CHECK(!is_valid_identifier("Users"));  // 소문자 정규화 후에만 유효
}

TEST(record_roundtrip) {
  Schema s = parse_schema(testing::kUsersSchema, "users").take();
  Row row = {int64_t{7}, std::string("한글"), int64_t{-3}, 2.5};
  std::string buf;
  encode_row(s, row, &buf);
  auto back = decode_row(s, buf.data(), buf.size());
  CHECK(back.ok());
  CHECK_EQ(std::get<std::string>(back.value()[1]), std::string("한글"));
  CHECK_EQ(std::get<int64_t>(back.value()[2]), int64_t{-3});
  CHECK_EQ(std::get<double>(back.value()[3]), 2.5);
  int64_t id = 0;
  CHECK(peek_id(buf.data(), buf.size(), &id));
  CHECK_EQ(id, int64_t{7});
}

TEST(record_decode_rejects_corruption) {
  Schema s = parse_schema(testing::kUsersSchema, "users").take();
  Row row = {int64_t{1}, std::string("abc"), int64_t{1}, 1.0};
  std::string buf;
  encode_row(s, row, &buf);
  CHECK(!decode_row(s, buf.data(), buf.size() - 1).ok());  // 잘림
  CHECK(!decode_row(s, buf.data(), 3).ok());
  std::string extra = buf + "x";
  CHECK(!decode_row(s, extra.data(), extra.size()).ok());  // 남는 바이트
}

TEST(heap_append_read_scan) {
  TempDir dir;
  auto h = HeapFile::open(dir.path() + "/h.bin");
  CHECK(h.ok());
  std::string framed;
  std::vector<std::string> payloads = {"", "a", std::string(1000, 'x'), "hello"};
  for (const auto& p : payloads) HeapFile::frame_record(p, &framed);
  CHECK(h.value()->append(framed).ok());
  CHECK_EQ(h.value()->size(), framed.size());
  std::vector<std::string> seen;
  std::vector<uint64_t> offsets;
  auto end = h.value()->scan([&](uint64_t off, const char* p, uint32_t n) {
    offsets.push_back(off);
    seen.emplace_back(p, n);
    return true;
  });
  CHECK(end.ok());
  CHECK_EQ(end.value(), framed.size());
  CHECK(seen == payloads);
  auto rec = h.value()->read_record(offsets[2]);
  CHECK(rec.ok() && rec.value() == payloads[2]);
  CHECK(!h.value()->read_record(framed.size() + 100).ok());
}

TEST(heap_scan_spans_chunks_and_large_records) {
  TempDir dir;
  auto h = HeapFile::open(dir.path() + "/h.bin");
  std::string framed;
  size_t count = 0;
  for (int i = 0; i < 3000; ++i, ++count) HeapFile::frame_record(std::string(3000, static_cast<char>('a' + i % 26)), &framed);
  HeapFile::frame_record(std::string(6 * 1024 * 1024, 'z'), &framed);  // 청크(4MB)보다 큰 레코드
  ++count;
  HeapFile::frame_record("tail", &framed);
  ++count;
  CHECK(h.value()->append(framed).ok());
  size_t n = 0;
  bool ok = true;
  auto end = h.value()->scan([&](uint64_t, const char* p, uint32_t len) {
    if (n < 3000 && (len != 3000 || p[0] != static_cast<char>('a' + n % 26))) ok = false;
    ++n;
    return true;
  });
  CHECK(end.ok());
  CHECK(ok);
  CHECK_EQ(n, count);
}

TEST(heap_scan_stops_on_callback_and_detects_torn_tail) {
  TempDir dir;
  std::string path = dir.path() + "/h.bin";
  {
    auto h = HeapFile::open(path);
    std::string framed;
    HeapFile::frame_record("one", &framed);
    HeapFile::frame_record("two", &framed);
    CHECK(h.value()->append(framed).ok());
  }
  // 찢어진 꼬리: 길이는 100이라고 하지만 3바이트만 존재
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
  uint32_t n = 100;
  CHECK(::write(fd, &n, 4) == 4);
  CHECK(::write(fd, "abc", 3) == 3);
  ::close(fd);
  auto h = HeapFile::open(path);
  size_t seen = 0;
  auto end = h.value()->scan([&](uint64_t, const char*, uint32_t) { ++seen; return true; });
  CHECK(end.ok());
  CHECK_EQ(seen, size_t{2});
  CHECK_EQ(end.value(), uint64_t{14});  // (4+3)*2
  seen = 0;
  auto stop = h.value()->scan([&](uint64_t, const char*, uint32_t) { ++seen; return false; });
  CHECK(stop.ok());
  CHECK_EQ(seen, size_t{1});
  // 비정상적으로 큰 길이는 손상으로 판정
  fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);
  uint32_t huge = 0xFFFFFFFFu;
  CHECK(::write(fd, &huge, 4) == 4);
  ::close(fd);
  auto h2 = HeapFile::open(path);
  CHECK(!h2.value()->scan([](uint64_t, const char*, uint32_t) { return true; }).ok());
}
