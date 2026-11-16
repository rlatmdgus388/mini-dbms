#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace minidb {

enum class ColumnType { kInt, kBigInt, kDouble, kVarchar };

struct Column {
  std::string name;  // 소문자로 정규화됨
  ColumnType type = ColumnType::kInt;
  uint32_t max_len = 0;  // VARCHAR(n)의 n (바이트 단위)
};

// 한 칸의 값. INT/BIGINT -> int64_t, DOUBLE -> double, VARCHAR -> string.
using Value = std::variant<int64_t, double, std::string>;
using Row = std::vector<Value>;

inline constexpr int64_t kIntMin = INT32_MIN;
inline constexpr int64_t kIntMax = INT32_MAX;

std::string value_to_string(const Value& v);
std::string type_to_string(const Column& c);
bool is_numeric(const Value& v);
// 같은 계열(숫자/숫자, 문자열/문자열)끼리의 비교. -1/0/1. 호출자가 계열 일치를 보장해야 한다.
int compare_values(const Value& a, const Value& b);

}  // namespace minidb
