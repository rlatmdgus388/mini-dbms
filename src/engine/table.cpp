#include "engine/table.h"

#include <algorithm>
#include <mutex>

#include "storage/record.h"

namespace minidb {

namespace {
bool check_op(int cmp, CompareOp op) {
  switch (op) {
    case CompareOp::kEq: return cmp == 0;
    case CompareOp::kNe: return cmp != 0;
    case CompareOp::kLt: return cmp < 0;
    case CompareOp::kLe: return cmp <= 0;
    case CompareOp::kGt: return cmp > 0;
    case CompareOp::kGe: return cmp >= 0;
  }
  return false;
}
}  // namespace

bool matches_all(const Row& row, const std::vector<ResolvedCondition>& conds) {
  for (const ResolvedCondition& c : conds) {
    if (!check_op(compare_values(row[c.column], c.value), c.op)) return false;
  }
  return true;
}

Result<std::unique_ptr<Table>> Table::open(const std::string& table_dir, const std::string& name) {
  Result<Schema> schema = load_schema(table_dir + "/schema.json", name);
  if (!schema.ok()) return schema.status();
  Result<std::unique_ptr<HeapFile>> heap = HeapFile::open(table_dir + "/data.bin");
  if (!heap.ok()) return heap.status();

  std::unique_ptr<Table> t(new Table());
  t->schema_ = schema.take();
  t->heap_ = heap.take();

  // 재시작 복구: 파일을 한 번 훑어 next_id, 행 수를 복원하고 B+ 트리 인덱스를 다시 만든다.
  int64_t max_id = 0;
  uint64_t count = 0;
  bool bad_record = false;
  Result<uint64_t> valid_end = t->heap_->scan([&](uint64_t offset, const char* p, uint32_t n) {
    int64_t id;
    if (!peek_id(p, n, &id) || id <= max_id) {  // id는 단조 증가해야 한다
      bad_record = true;
      return false;
    }
    t->index_.insert(id, offset);  // 인덱스 재구축 (id 오름차순이라 항상 우측 끝에 삽입)
    max_id = id;
    ++count;
    return true;
  });
  if (!valid_end.ok()) return valid_end.status();
  if (bad_record) return Status::fail(ErrorCode::kIo, "corrupt data file for table '" + name + "'");
  if (valid_end.value() < t->heap_->size()) {  // 쓰기 도중 중단된 꼬리 레코드 제거
    Status st = t->heap_->truncate_to(valid_end.value());
    if (!st.ok()) return st;
  }
  t->next_id_ = max_id + 1;
  t->row_count_ = count;
  return t;
}

Result<InsertInfo> Table::insert(std::vector<Row> rows) {
  if (rows.empty()) return Status::fail(ErrorCode::kInvalid, "no rows to insert");
  std::string buffer;
  std::string payload;
  std::unique_lock<std::shared_mutex> lock(mutex_);  // id 발급 -> append 가 하나의 임계구역
  InsertInfo info;
  info.first_id = next_id_;
  int64_t id = next_id_;
  const uint64_t base = heap_->size();
  std::vector<uint64_t> offsets;
  offsets.reserve(rows.size());
  for (Row& row : rows) {
    row[0] = id++;
    payload.clear();
    encode_row(schema_, row, &payload);
    if (payload.size() > HeapFile::kMaxRecordSize)
      return Status::fail(ErrorCode::kInvalid, "row too large");
    offsets.push_back(base + buffer.size());
    HeapFile::frame_record(payload, &buffer);
  }
  Status st = heap_->append(buffer);
  if (!st.ok()) return st;
  for (size_t i = 0; i < offsets.size(); ++i) index_.insert(info.first_id + static_cast<int64_t>(i), offsets[i]);
  next_id_ = id;
  row_count_ += rows.size();
  info.last_id = id - 1;
  info.count = rows.size();
  return info;
}

namespace {
// WHERE 절에서 id 컬럼의 정수 비교(!= 제외)를 모아 [lo, hi] 범위로 만든다.
// 사용할 조건이 하나도 없으면 false. 범위가 비면 *empty = true.
bool extract_id_range(const std::vector<ResolvedCondition>& conds, int64_t* lo, int64_t* hi, bool* empty) {
  bool used = false;
  *lo = INT64_MIN;
  *hi = INT64_MAX;
  *empty = false;
  for (const ResolvedCondition& c : conds) {
    if (c.column != 0 || c.op == CompareOp::kNe) continue;
    const int64_t* v = std::get_if<int64_t>(&c.value);
    if (!v) continue;  // 실수 리터럴 등은 인덱스를 쓰지 않고 필터로만 평가한다
    used = true;
    switch (c.op) {
      case CompareOp::kEq: *lo = std::max(*lo, *v); *hi = std::min(*hi, *v); break;
      case CompareOp::kGe: *lo = std::max(*lo, *v); break;
      case CompareOp::kLe: *hi = std::min(*hi, *v); break;
      case CompareOp::kGt:
        if (*v == INT64_MAX) *empty = true; else *lo = std::max(*lo, *v + 1);
        break;
      case CompareOp::kLt:
        if (*v == INT64_MIN) *empty = true; else *hi = std::min(*hi, *v - 1);
        break;
      case CompareOp::kNe: break;
    }
  }
  if (*lo > *hi) *empty = true;
  return used;
}
}  // namespace

Result<SelectOutput> Table::select(const std::vector<size_t>& projection, const std::vector<ResolvedCondition>& conds,
                                   std::optional<uint64_t> limit) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  SelectOutput out;
  Status error = Status::success();

  // 행 하나를 필터/투영해서 결과에 담는다. 더 받을 수 있으면 true.
  auto consume = [&](const char* p, uint32_t n) {
    Result<Row> row = decode_row(schema_, p, n);
    if (!row.ok()) {
      error = row.status();
      return false;
    }
    if (!matches_all(row.value(), conds)) return true;
    Row projected;
    projected.reserve(projection.size());
    for (size_t c : projection) projected.push_back(std::move(row.value()[c]));
    out.rows.push_back(std::move(projected));
    return !(limit && out.rows.size() >= *limit);
  };

  int64_t lo, hi;
  bool empty;
  if (extract_id_range(conds, &lo, &hi, &empty)) {
    // 실행 계획: id 조건 -> B+ 트리 (점 조회 O(log n), 범위는 리프 연결 리스트 순회)
    out.plan = "INDEX";
    if (empty || (limit && *limit == 0)) return out;
    index_.range(lo, hi, [&](int64_t, uint64_t offset) {
      Result<std::string> rec = heap_->read_record(offset);
      if (!rec.ok()) {
        error = rec.status();
        return false;
      }
      return consume(rec.value().data(), static_cast<uint32_t>(rec.value().size()));
    });
    if (!error.ok()) return error;
    return out;
  }

  out.plan = "SCAN";  // 그 외 컬럼 조건은 전체 선형 탐색
  if (limit && *limit == 0) return out;
  Result<uint64_t> scanned = heap_->scan([&](uint64_t, const char* p, uint32_t n) { return consume(p, n); });
  if (!error.ok()) return error;
  if (!scanned.ok()) return scanned.status();
  return out;
}

uint64_t Table::row_count() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return row_count_;
}

Status Table::verify_index() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  Status st = index_.validate();
  if (!st.ok()) return st;
  if (index_.size() != row_count_)
    return Status::fail(ErrorCode::kInternal, "index size " + std::to_string(index_.size()) + " != row count " + std::to_string(row_count_));
  uint64_t seen = 0;
  Status mismatch = Status::success();
  Result<uint64_t> scanned = heap_->scan([&](uint64_t offset, const char* p, uint32_t n) {
    int64_t id;
    if (!peek_id(p, n, &id)) {
      mismatch = Status::fail(ErrorCode::kInternal, "unreadable record id");
      return false;
    }
    std::optional<uint64_t> off = index_.find(id);
    if (!off || *off != offset) {
      mismatch = Status::fail(ErrorCode::kInternal, "index entry for id " + std::to_string(id) + " does not match heap offset");
      return false;
    }
    ++seen;
    return true;
  });
  if (!mismatch.ok()) return mismatch;
  if (!scanned.ok()) return scanned.status();
  if (seen != row_count_) return Status::fail(ErrorCode::kInternal, "heap record count mismatch");
  return Status::success();
}

}  // namespace minidb
