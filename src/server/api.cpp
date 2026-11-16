#include "server/api.h"

#include <cstdio>

#include "common/json.h"

namespace minidb {

namespace {
void append_value(std::string* out, const Value& v) {
  if (const auto* i = std::get_if<int64_t>(&v)) {
    *out += std::to_string(*i);
  } else if (const auto* d = std::get_if<double>(&v)) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.15g", *d);
    *out += buf;
  } else {
    *out += '"';
    *out += json_escape(std::get<std::string>(v));
    *out += '"';
  }
}

void append_result(std::string* out, const QueryResult& r) {
  *out += "{\"kind\":\"" + r.kind + "\"";
  if (r.kind == "INSERT") {
    *out += ",\"affected\":" + std::to_string(r.affected) + ",\"first_id\":" + std::to_string(r.first_id) +
            ",\"last_id\":" + std::to_string(r.last_id) + "}";
    return;
  }
  *out += ",\"columns\":[";
  for (size_t i = 0; i < r.columns.size(); ++i) *out += (i ? ",\"" : "\"") + json_escape(r.columns[i]) + "\"";
  *out += "],\"rows\":[";
  for (size_t i = 0; i < r.rows.size(); ++i) {
    if (i) *out += ',';
    *out += '[';
    for (size_t j = 0; j < r.rows[i].size(); ++j) {
      if (j) *out += ',';
      append_value(out, r.rows[i][j]);
    }
    *out += ']';
  }
  *out += "],\"plan\":\"" + r.plan + "\",\"count\":" + std::to_string(r.rows.size()) + "}";
}
}  // namespace

std::string outcome_to_json(const ExecOutcome& outcome, double elapsed_ms) {
  std::string out = outcome.status.ok() ? "{\"ok\":true" : "{\"ok\":false,\"error\":\"" + json_escape(outcome.status.message()) + "\"";
  out += ",\"results\":[";
  for (size_t i = 0; i < outcome.results.size(); ++i) {
    if (i) out += ',';
    append_result(&out, outcome.results[i]);
  }
  char buf[48];
  std::snprintf(buf, sizeof(buf), "],\"elapsed_ms\":%.3f}", elapsed_ms);
  out += buf;
  return out;
}

std::string error_to_json(const std::string& message) {
  return "{\"ok\":false,\"error\":\"" + json_escape(message) + "\"}";
}

int http_status_for(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk: return 200;
    case ErrorCode::kParse:
    case ErrorCode::kInvalid: return 400;
    case ErrorCode::kNotFound: return 404;
    case ErrorCode::kIo:
    case ErrorCode::kInternal: return 500;
  }
  return 500;
}

Result<std::string> extract_sql(const std::string& content_type, const std::string& body) {
  size_t first = body.find_first_not_of(" \t\r\n");
  bool looks_json = content_type.find("json") != std::string::npos || (first != std::string::npos && body[first] == '{');
  std::string sql = body;
  if (looks_json) {
    Result<JsonValue> parsed = parse_json(body);
    if (!parsed.ok()) return Status::fail(ErrorCode::kParse, parsed.status().message());
    const JsonValue* s = parsed.value().type == JsonValue::Type::kObject ? parsed.value().find("sql") : nullptr;
    if (!s || s->type != JsonValue::Type::kString) return Status::fail(ErrorCode::kInvalid, "JSON body must be {\"sql\": \"...\"}");
    sql = s->string;
  }
  if (sql.find_first_not_of(" \t\r\n") == std::string::npos) return Status::fail(ErrorCode::kInvalid, "empty SQL");
  return sql;
}

}  // namespace minidb
