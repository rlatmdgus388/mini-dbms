#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "common/result.h"
#include "index/bplus_tree.h"
#include "sql/ast.h"
#include "storage/heap_file.h"
#include "storage/schema.h"

namespace minidb {

// 컬럼 이름이 인덱스로 해석되고 값이 컬럼 타입 계열에 맞게 검증된 조건.
struct ResolvedCondition {
  size_t column = 0;
  CompareOp op = CompareOp::kEq;
  Value value;
};

struct InsertInfo {
  int64_t first_id = 0;
  int64_t last_id = 0;
  uint64_t count = 0;
};

struct SelectOutput {
  std::vector<Row> rows;
  std::string plan;  // "INDEX" (id 조건으로 B+ 트리 사용) | "SCAN" (전체 선형 탐색)
};

// 테이블 하나 = 스키마 + 힙 파일 + id 인덱스(B+ 트리: id -> 파일 offset). 모든 데이터 접근은 mutex_ 로 보호된다 (CLAUDE.md 절대 규칙 3):
//   SELECT = shared lock, INSERT = unique lock.
class Table {
 public:
  static Result<std::unique_ptr<Table>> open(const std::string& table_dir, const std::string& name);

  const Schema& schema() const { return schema_; }

  // rows[i][0]은 id 자리표시자이며 insert가 배타 락 안에서 실제 id로 채운다.
  // 나머지 컬럼은 스키마 타입에 맞게 이미 coerce/검증되어 있어야 한다. 전부 성공하거나 전부 실패한다.
  Result<InsertInfo> insert(std::vector<Row> rows);

  // projection: 출력할 컬럼 인덱스. limit 이 있으면 그만큼만 반환한다.
  Result<SelectOutput> select(const std::vector<size_t>& projection, const std::vector<ResolvedCondition>& conds,
                              std::optional<uint64_t> limit) const;

  uint64_t row_count() const;

  // 인덱스 불변식 + 힙 파일과의 일치(모든 레코드의 id/offset)를 검증한다 (테스트/진단용).
  Status verify_index() const;

 private:
  Table() = default;

  Schema schema_;
  std::unique_ptr<HeapFile> heap_;
  BPlusTree<int64_t, uint64_t, 128> index_;
  int64_t next_id_ = 1;
  uint64_t row_count_ = 0;
  mutable std::shared_mutex mutex_;
};

bool matches_all(const Row& row, const std::vector<ResolvedCondition>& conds);

}  // namespace minidb
