#pragma once
#include <string>
#include <vector>

#include "common/result.h"

namespace minidb {

// 스키마 파일과 API 요청 본문을 읽기 위한 최소 JSON 구현.
struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<JsonValue> array;
  std::vector<std::string> keys;     // object의 키 (values와 같은 인덱스)
  std::vector<JsonValue> values;

  const JsonValue* find(const std::string& key) const;
};

Result<JsonValue> parse_json(const std::string& text);
// 따옴표를 제외한 JSON 문자열 이스케이프.
std::string json_escape(const std::string& s);

}  // namespace minidb
