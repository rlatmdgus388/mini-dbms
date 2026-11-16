#include "sql/parser.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>

#include "sql/lexer.h"

namespace minidb {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : t_(std::move(tokens)) {}

  Result<std::vector<Statement>> parse_all() {
    std::vector<Statement> out;
    while (true) {
      while (is_symbol(";")) ++i_;
      if (cur().type == TokenType::kEnd) break;
      Result<Statement> stmt = parse_statement();
      if (!stmt.ok()) return stmt.status();
      out.push_back(stmt.take());
      if (cur().type != TokenType::kEnd && !is_symbol(";")) return err("expected ';' or end of input");
    }
    return out;
  }

 private:
  const Token& cur() const { return t_[i_]; }
  bool is_symbol(const char* s) const { return cur().type == TokenType::kSymbol && cur().text == s; }
  bool is_keyword(const char* kw) const { return cur().type == TokenType::kIdent && upper(cur().text) == kw; }

  Status err(const std::string& msg) const {
    std::string near = cur().type == TokenType::kEnd ? "end of input" : "'" + cur().text + "'";
    return Status::fail(ErrorCode::kParse, "syntax error at position " + std::to_string(cur().pos) + " near " + near +
                                               ": " + msg);
  }

  bool accept_keyword(const char* kw) {
    if (!is_keyword(kw)) return false;
    ++i_;
    return true;
  }
  bool accept_symbol(const char* s) {
    if (!is_symbol(s)) return false;
    ++i_;
    return true;
  }
  Status expect_keyword(const char* kw) {
    if (accept_keyword(kw)) return Status::success();
    return err(std::string("expected ") + kw);
  }
  Status expect_symbol(const char* s) {
    if (accept_symbol(s)) return Status::success();
    return err(std::string("expected '") + s + "'");
  }
  Result<std::string> parse_identifier(const char* what) {
    // 예약어는 식별자로 쓸 수 없다 (SELECT FROM t 가 컬럼 "from" 으로 오해석되는 것을 방지).
    static const char* const kReserved[] = {"SELECT", "INSERT", "INTO", "VALUES", "FROM", "WHERE", "AND", "LIMIT", "BETWEEN"};
    if (cur().type != TokenType::kIdent) return err(std::string("expected ") + what);
    std::string up = upper(cur().text);
    for (const char* kw : kReserved)
      if (up == kw) return err(std::string("expected ") + what);
    return lower(t_[i_++].text);
  }

  Result<Value> parse_literal() {
    bool negative = false;
    bool signed_num = false;
    if (is_symbol("-") || is_symbol("+")) {
      negative = cur().text == "-";
      signed_num = true;
      ++i_;
    }
    const Token& tok = cur();
    if (tok.type == TokenType::kString && !signed_num) {
      ++i_;
      return Value(tok.text);
    }
    if (tok.type == TokenType::kInteger) {
      std::string text = (negative ? "-" : "") + tok.text;
      errno = 0;
      char* end = nullptr;
      long long v = std::strtoll(text.c_str(), &end, 10);
      if (errno == ERANGE) return err("integer literal out of range");
      ++i_;
      return Value(static_cast<int64_t>(v));
    }
    if (tok.type == TokenType::kFloat) {
      std::string text = (negative ? "-" : "") + tok.text;
      double d = std::strtod(text.c_str(), nullptr);
      if (!std::isfinite(d)) return err("numeric literal out of range");
      ++i_;
      return Value(d);
    }
    if (tok.type == TokenType::kIdent && upper(tok.text) == "NULL")
      return Status::fail(ErrorCode::kInvalid, "NULL is not supported");
    return err("expected a literal value");
  }

  Result<Statement> parse_statement() {
    if (is_keyword("INSERT")) return parse_insert();
    if (is_keyword("SELECT")) return parse_select();
    return err("expected INSERT or SELECT");
  }

  Result<Statement> parse_insert() {
    ++i_;  // INSERT
    Status st = expect_keyword("INTO");
    if (!st.ok()) return st;
    Result<std::string> table = parse_identifier("table name");
    if (!table.ok()) return table.status();
    InsertStmt stmt;
    stmt.table = table.take();
    if (accept_symbol("(")) {
      while (true) {
        Result<std::string> col = parse_identifier("column name");
        if (!col.ok()) return col.status();
        stmt.columns.push_back(col.take());
        if (accept_symbol(",")) continue;
        st = expect_symbol(")");
        if (!st.ok()) return st;
        break;
      }
    }
    st = expect_keyword("VALUES");
    if (!st.ok()) return st;
    while (true) {
      st = expect_symbol("(");
      if (!st.ok()) return st;
      std::vector<Value> row;
      while (true) {
        Result<Value> v = parse_literal();
        if (!v.ok()) return v.status();
        row.push_back(v.take());
        if (accept_symbol(",")) continue;
        st = expect_symbol(")");
        if (!st.ok()) return st;
        break;
      }
      stmt.rows.push_back(std::move(row));
      if (!accept_symbol(",")) break;
    }
    return Statement(std::move(stmt));
  }

  Result<Statement> parse_select() {
    ++i_;  // SELECT
    SelectStmt stmt;
    if (accept_symbol("*")) {
      stmt.star = true;
    } else {
      while (true) {
        Result<std::string> col = parse_identifier("column name or '*'");
        if (!col.ok()) return col.status();
        stmt.columns.push_back(col.take());
        if (!accept_symbol(",")) break;
      }
    }
    Status st = expect_keyword("FROM");
    if (!st.ok()) return st;
    Result<std::string> table = parse_identifier("table name");
    if (!table.ok()) return table.status();
    stmt.table = table.take();
    if (accept_keyword("WHERE")) {
      while (true) {
        st = parse_condition(&stmt.where);
        if (!st.ok()) return st;
        if (!accept_keyword("AND")) break;
      }
    }
    if (accept_keyword("LIMIT")) {
      if (cur().type != TokenType::kInteger) return err("LIMIT requires a non-negative integer");
      errno = 0;
      unsigned long long v = std::strtoull(cur().text.c_str(), nullptr, 10);
      if (errno == ERANGE) return err("LIMIT out of range");
      ++i_;
      stmt.limit = static_cast<uint64_t>(v);
    }
    return Statement(std::move(stmt));
  }

  Status parse_condition(std::vector<Condition>* out) {
    Result<std::string> col = parse_identifier("column name");
    if (!col.ok()) return col.status();
    if (accept_keyword("BETWEEN")) {
      Result<Value> lo = parse_literal();
      if (!lo.ok()) return lo.status();
      Status st = expect_keyword("AND");
      if (!st.ok()) return st;
      Result<Value> hi = parse_literal();
      if (!hi.ok()) return hi.status();
      out->push_back({col.value(), CompareOp::kGe, lo.take()});
      out->push_back({col.value(), CompareOp::kLe, hi.take()});
      return Status::success();
    }
    CompareOp op;
    if (accept_symbol("=")) op = CompareOp::kEq;
    else if (accept_symbol("!=")) op = CompareOp::kNe;
    else if (accept_symbol("<=")) op = CompareOp::kLe;
    else if (accept_symbol(">=")) op = CompareOp::kGe;
    else if (accept_symbol("<")) op = CompareOp::kLt;
    else if (accept_symbol(">")) op = CompareOp::kGt;
    else return err("expected comparison operator");
    Result<Value> v = parse_literal();
    if (!v.ok()) return v.status();
    out->push_back({col.value(), op, v.take()});
    return Status::success();
  }

  std::vector<Token> t_;
  size_t i_ = 0;
};

}  // namespace

Result<std::vector<Statement>> parse_script(const std::string& sql) {
  Result<std::vector<Token>> tokens = tokenize(sql);
  if (!tokens.ok()) return tokens.status();
  return Parser(tokens.take()).parse_all();
}

}  // namespace minidb
