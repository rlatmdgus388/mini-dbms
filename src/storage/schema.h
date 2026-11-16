#pragma once
#include <string>
#include <vector>

#include "common/result.h"
#include "common/value.h"

namespace minidb {

// 테이블 스키마. 첫 컬럼은 항상 자동 부여되는 `id BIGINT` 이다.
struct Schema {
  std::string table;
  std::vector<Column> columns;

  // 소문자 컬럼명 -> 인덱스, 없으면 -1.
  int column_index(const std::string& name) const;
};

// 테이블/컬럼 이름 규칙: [a-z_][a-z0-9_]* (경로 탈출 방지를 위해 엄격히 제한)
bool is_valid_identifier(const std::string& name);

// `<data_dir>/<table>/schema.json` 형식:
//   {"table":"users","columns":[{"name":"id","type":"BIGINT"},{"name":"name","type":"VARCHAR(50)"}]}
Result<Schema> parse_schema(const std::string& json_text, const std::string& expected_table);
Result<Schema> load_schema(const std::string& path, const std::string& expected_table);

}  // namespace minidb
