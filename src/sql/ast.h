#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/value.h"

namespace minidb {

enum class CompareOp { kEq, kNe, kLt, kLe, kGt, kGe };

// `col op literal`. BETWEEN a AND b 는 파서가 (>= a), (<= b) 두 조건으로 풀어 놓는다.
struct Condition {
  std::string column;  // 소문자
  CompareOp op = CompareOp::kEq;
  Value value;
};

struct InsertStmt {
  std::string table;
  std::vector<std::string> columns;  // 비어 있으면 id를 제외한 모든 컬럼 (스키마 순서)
  std::vector<std::vector<Value>> rows;
};

struct SelectStmt {
  std::string table;
  bool star = false;
  std::vector<std::string> columns;
  std::vector<Condition> where;  // AND로만 결합
  std::optional<uint64_t> limit;
};

using Statement = std::variant<InsertStmt, SelectStmt>;

}  // namespace minidb
