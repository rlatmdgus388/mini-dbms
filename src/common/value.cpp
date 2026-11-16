#include "common/value.h"

#include <cstdio>

namespace minidb {

std::string value_to_string(const Value& v) {
  if (const auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
  if (const auto* d = std::get_if<double>(&v)) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.15g", *d);
    return buf;
  }
  return std::get<std::string>(v);
}

std::string type_to_string(const Column& c) {
  switch (c.type) {
    case ColumnType::kInt: return "INT";
    case ColumnType::kBigInt: return "BIGINT";
    case ColumnType::kDouble: return "DOUBLE";
    case ColumnType::kVarchar: return "VARCHAR(" + std::to_string(c.max_len) + ")";
  }
  return "?";
}

bool is_numeric(const Value& v) { return !std::holds_alternative<std::string>(v); }

int compare_values(const Value& a, const Value& b) {
  if (const auto* sa = std::get_if<std::string>(&a)) {
    const auto& sb = std::get<std::string>(b);
    int c = sa->compare(sb);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  // 정수끼리는 정확히 비교한다 (double 변환 시 정밀도 손실 방지).
  if (std::holds_alternative<int64_t>(a) && std::holds_alternative<int64_t>(b)) {
    int64_t x = std::get<int64_t>(a), y = std::get<int64_t>(b);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  auto to_d = [](const Value& v) {
    return std::holds_alternative<int64_t>(v) ? static_cast<double>(std::get<int64_t>(v))
                                              : std::get<double>(v);
  };
  double x = to_d(a), y = to_d(b);
  return x < y ? -1 : (x > y ? 1 : 0);
}

}  // namespace minidb
