#include "engine/engine.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "sql/parser.h"

namespace minidb {

namespace {

Status invalid(const std::string& msg) { return Status::fail(ErrorCode::kInvalid, msg); }

// 리터럴을 컬럼 타입에 맞는 값으로 변환한다. INT 범위, VARCHAR 길이도 여기서 검증한다.
Result<Value> coerce_for_insert(const Column& col, const Value& v) {
  switch (col.type) {
    case ColumnType::kInt:
    case ColumnType::kBigInt: {
      const auto* i = std::get_if<int64_t>(&v);
      if (!i) return invalid("column '" + col.name + "' expects an integer");
      if (col.type == ColumnType::kInt && (*i < kIntMin || *i > kIntMax))
        return invalid("value out of range for INT column '" + col.name + "'");
      return Value(*i);
    }
    case ColumnType::kDouble: {
      if (const auto* i = std::get_if<int64_t>(&v)) return Value(static_cast<double>(*i));
      if (const auto* d = std::get_if<double>(&v)) return Value(*d);
      return invalid("column '" + col.name + "' expects a number");
    }
    case ColumnType::kVarchar: {
      const auto* s = std::get_if<std::string>(&v);
      if (!s) return invalid("column '" + col.name + "' expects a string");
      if (s->size() > col.max_len)
        return invalid("string too long for column '" + col.name + "' (max " + std::to_string(col.max_len) + " bytes)");
      return Value(*s);
    }
  }
  return invalid("unsupported column type");
}

}  // namespace

Result<std::unique_ptr<Engine>> Engine::open(const std::string& data_dir) {
  struct stat st;
  if (::stat(data_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
    return Status::fail(ErrorCode::kNotFound, "data directory '" + data_dir + "' does not exist");
  return std::unique_ptr<Engine>(new Engine(data_dir));
}

Result<Table*> Engine::get_table(const std::string& name) {
  // 테이블 이름은 경로로 쓰이므로 엄격히 검증한다 (../ 등 경로 탈출 방지).
  if (!is_valid_identifier(name)) return invalid("invalid table name '" + name + "'");
  std::lock_guard<std::mutex> lock(tables_mutex_);
  auto it = tables_.find(name);
  if (it != tables_.end()) return it->second.get();
  Result<std::unique_ptr<Table>> t = Table::open(data_dir_ + "/" + name, name);
  if (!t.ok()) return t.status();
  Table* raw = t.value().get();
  tables_[name] = t.take();
  return raw;
}

Status Engine::load_all_tables() {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir_, ec)) {
    if (!entry.is_directory()) continue;
    std::string name = entry.path().filename().string();
    if (!is_valid_identifier(name)) continue;
    if (!std::filesystem::exists(entry.path() / "schema.json")) continue;
    Result<Table*> t = get_table(name);
    if (!t.ok()) return t.status();
  }
  if (ec) return Status::fail(ErrorCode::kIo, "cannot list data directory: " + ec.message());
  return Status::success();
}

ExecOutcome Engine::execute(const std::string& sql) {
  ExecOutcome outcome;
  Result<std::vector<Statement>> stmts = parse_script(sql);
  if (!stmts.ok()) {
    outcome.status = stmts.status();
    return outcome;
  }
  for (const Statement& stmt : stmts.value()) {
    Result<QueryResult> r = execute_statement(stmt);
    if (!r.ok()) {
      outcome.status = r.status();
      return outcome;
    }
    outcome.results.push_back(r.take());
  }
  return outcome;
}

Result<QueryResult> Engine::execute_statement(const Statement& stmt) {
  if (const auto* ins = std::get_if<InsertStmt>(&stmt)) return execute_insert(*ins);
  return execute_select(std::get<SelectStmt>(stmt));
}

Result<QueryResult> Engine::execute_insert(const InsertStmt& stmt) {
  Result<Table*> table = get_table(stmt.table);
  if (!table.ok()) return table.status();
  const Schema& schema = table.value()->schema();

  // 컬럼 목록 -> 스키마 인덱스. 생략하면 id 를 제외한 전체 컬럼.
  std::vector<size_t> targets;
  if (stmt.columns.empty()) {
    for (size_t i = 1; i < schema.columns.size(); ++i) targets.push_back(i);
  } else {
    std::vector<bool> seen(schema.columns.size(), false);
    for (const std::string& name : stmt.columns) {
      int idx = schema.column_index(name);
      if (idx < 0) return invalid("unknown column '" + name + "' in table '" + stmt.table + "'");
      if (idx == 0) return invalid("column 'id' is auto-assigned and cannot be specified");
      if (seen[static_cast<size_t>(idx)]) return invalid("duplicate column '" + name + "'");
      seen[static_cast<size_t>(idx)] = true;
      targets.push_back(static_cast<size_t>(idx));
    }
    if (targets.size() != schema.columns.size() - 1)
      return invalid("all columns must be specified (NULL is not supported)");
  }

  std::vector<Row> rows;
  rows.reserve(stmt.rows.size());
  for (const auto& values : stmt.rows) {
    if (values.size() != targets.size())
      return invalid("expected " + std::to_string(targets.size()) + " values but got " + std::to_string(values.size()));
    Row row(schema.columns.size());
    row[0] = int64_t{0};  // id 자리표시자
    for (size_t k = 0; k < targets.size(); ++k) {
      Result<Value> v = coerce_for_insert(schema.columns[targets[k]], values[k]);
      if (!v.ok()) return v.status();
      row[targets[k]] = v.take();
    }
    rows.push_back(std::move(row));
  }

  Result<InsertInfo> info = table.value()->insert(std::move(rows));
  if (!info.ok()) return info.status();
  QueryResult out;
  out.kind = "INSERT";
  out.affected = info.value().count;
  out.first_id = info.value().first_id;
  out.last_id = info.value().last_id;
  return out;
}

Result<QueryResult> Engine::execute_select(const SelectStmt& stmt) {
  Result<Table*> table = get_table(stmt.table);
  if (!table.ok()) return table.status();
  const Schema& schema = table.value()->schema();

  std::vector<size_t> projection;
  QueryResult out;
  out.kind = "SELECT";
  if (stmt.star) {
    for (size_t i = 0; i < schema.columns.size(); ++i) projection.push_back(i);
  } else {
    for (const std::string& name : stmt.columns) {
      int idx = schema.column_index(name);
      if (idx < 0) return invalid("unknown column '" + name + "' in table '" + stmt.table + "'");
      projection.push_back(static_cast<size_t>(idx));
    }
  }
  for (size_t c : projection) out.columns.push_back(schema.columns[c].name);

  std::vector<ResolvedCondition> conds;
  for (const Condition& c : stmt.where) {
    int idx = schema.column_index(c.column);
    if (idx < 0) return invalid("unknown column '" + c.column + "' in WHERE clause");
    const Column& col = schema.columns[static_cast<size_t>(idx)];
    bool col_is_string = col.type == ColumnType::kVarchar;
    bool val_is_string = std::holds_alternative<std::string>(c.value);
    if (col_is_string != val_is_string)
      return invalid("cannot compare column '" + col.name + "' (" + type_to_string(col) + ") with a " +
                     (val_is_string ? "string" : "number"));
    conds.push_back({static_cast<size_t>(idx), c.op, c.value});
  }

  Result<SelectOutput> sel = table.value()->select(projection, conds, stmt.limit);
  if (!sel.ok()) return sel.status();
  out.rows = std::move(sel.value().rows);
  out.plan = sel.value().plan;
  return out;
}

}  // namespace minidb
