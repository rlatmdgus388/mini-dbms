#include "storage/schema.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "common/json.h"

namespace minidb {

namespace {
Status bad(const std::string& msg) { return Status::fail(ErrorCode::kInvalid, "invalid schema: " + msg); }

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

Result<Column> parse_type(const std::string& name, const std::string& type_text) {
  std::string t;
  for (char c : type_text)
    if (!std::isspace(static_cast<unsigned char>(c))) t.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  Column col;
  col.name = name;
  if (t == "INT") { col.type = ColumnType::kInt; return col; }
  if (t == "BIGINT") { col.type = ColumnType::kBigInt; return col; }
  if (t == "DOUBLE") { col.type = ColumnType::kDouble; return col; }
  if (t.rfind("VARCHAR(", 0) == 0 && t.back() == ')') {
    std::string num = t.substr(8, t.size() - 9);
    if (num.empty() || num.size() > 5 || !std::all_of(num.begin(), num.end(), [](char c) { return c >= '0' && c <= '9'; }))
      return bad("column '" + name + "': bad VARCHAR length");
    int n = std::stoi(num);
    if (n < 1 || n > 65535) return bad("column '" + name + "': VARCHAR length must be 1..65535");
    col.type = ColumnType::kVarchar;
    col.max_len = static_cast<uint32_t>(n);
    return col;
  }
  return bad("column '" + name + "': unsupported type '" + type_text + "'");
}
}  // namespace

int Schema::column_index(const std::string& name) const {
  for (size_t i = 0; i < columns.size(); ++i)
    if (columns[i].name == name) return static_cast<int>(i);
  return -1;
}

bool is_valid_identifier(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  if (!(std::islower(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_';
  });
}

Result<Schema> parse_schema(const std::string& json_text, const std::string& expected_table) {
  Result<JsonValue> parsed = parse_json(json_text);
  if (!parsed.ok()) return Status::fail(ErrorCode::kInvalid, "invalid schema: " + parsed.status().message());
  const JsonValue& root = parsed.value();
  if (root.type != JsonValue::Type::kObject) return bad("top-level value must be an object");

  Schema schema;
  schema.table = expected_table;
  if (const JsonValue* t = root.find("table")) {
    if (t->type != JsonValue::Type::kString || to_lower(t->string) != expected_table)
      return bad("\"table\" does not match directory name '" + expected_table + "'");
  }
  const JsonValue* cols = root.find("columns");
  if (!cols || cols->type != JsonValue::Type::kArray || cols->array.empty()) return bad("\"columns\" must be a non-empty array");

  for (const JsonValue& c : cols->array) {
    const JsonValue* n = c.type == JsonValue::Type::kObject ? c.find("name") : nullptr;
    const JsonValue* ty = c.type == JsonValue::Type::kObject ? c.find("type") : nullptr;
    if (!n || !ty || n->type != JsonValue::Type::kString || ty->type != JsonValue::Type::kString)
      return bad("each column needs string \"name\" and \"type\"");
    std::string name = to_lower(n->string);
    if (!is_valid_identifier(name)) return bad("bad column name '" + n->string + "'");
    if (schema.column_index(name) >= 0) return bad("duplicate column '" + name + "'");
    Result<Column> col = parse_type(name, ty->string);
    if (!col.ok()) return col.status();
    schema.columns.push_back(col.take());
  }
  if (schema.columns[0].name != "id" || schema.columns[0].type != ColumnType::kBigInt)
    return bad("first column must be 'id BIGINT'");
  return schema;
}

Result<Schema> load_schema(const std::string& path, const std::string& expected_table) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Status::fail(ErrorCode::kNotFound, "table '" + expected_table + "' does not exist");
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_schema(ss.str(), expected_table);
}

}  // namespace minidb
