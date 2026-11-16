#include "common/json.h"
#include "sql/lexer.h"
#include "sql/parser.h"
#include "test_util.h"

using namespace minidb;

TEST(json_parses_nested_and_escapes) {
  auto r = parse_json(R"J({"a":[1,2.5,"x\n\u00e9\ud83d\ude00"],"b":{"c":true,"d":null}})J");
  CHECK(r.ok());
  const JsonValue* a = r.value().find("a");
  CHECK(a && a->array.size() == 3);
  CHECK_EQ(a->array[2].string, std::string("x\n\xC3\xA9\xF0\x9F\x98\x80"));
  CHECK(r.value().find("b")->find("c")->boolean);
}

TEST(json_rejects_malformed) {
  CHECK(!parse_json("").ok());
  CHECK(!parse_json("{").ok());
  CHECK(!parse_json("{\"a\":}").ok());
  CHECK(!parse_json("[1,2,]").ok());
  CHECK(!parse_json("\"abc").ok());
  CHECK(!parse_json("{} extra").ok());
  CHECK(!parse_json("\"\\ud800\"").ok());
  CHECK(!parse_json(std::string(200, '[')).ok());  // 깊이 제한
}

TEST(json_escape_roundtrip) {
  std::string s = "a\"b\\c\n\t\x01";
  auto r = parse_json("\"" + json_escape(s) + "\"");
  CHECK(r.ok());
  CHECK_EQ(r.value().string, s);
}

TEST(lexer_basic_tokens) {
  auto r = tokenize("SELECT a, b FROM t WHERE x >= -1.5e2 AND y <> 'it''s'; -- comment\n");
  CHECK(r.ok());
  const auto& t = r.value();
  CHECK_EQ(t.front().text, std::string("SELECT"));
  bool saw_ne = false, saw_str = false, saw_float = false;
  for (const auto& tok : t) {
    if (tok.type == TokenType::kSymbol && tok.text == "!=") saw_ne = true;
    if (tok.type == TokenType::kString && tok.text == "it's") saw_str = true;
    if (tok.type == TokenType::kFloat && tok.text == "1.5e2") saw_float = true;
  }
  CHECK(saw_ne && saw_str && saw_float);
  CHECK(t.back().type == TokenType::kEnd);
}

TEST(lexer_errors) {
  CHECK_ERR(tokenize("SELECT 'abc").status(), "unterminated string");
  CHECK_ERR(tokenize("SELECT @").status(), "unexpected character");
  CHECK_ERR(tokenize("SELECT 12abc").status(), "invalid number");
}

TEST(lexer_empty_and_comment_only) {
  auto r = tokenize("   -- nothing\n");
  CHECK(r.ok());
  CHECK_EQ(r.value().size(), size_t{1});
}

TEST(parser_insert_multi_row) {
  auto r = parse_script("insert into Users (name, age) values ('a', 1), ('b', -2);");
  CHECK(r.ok());
  CHECK_EQ(r.value().size(), size_t{1});
  const auto& ins = std::get<InsertStmt>(r.value()[0]);
  CHECK_EQ(ins.table, std::string("users"));
  CHECK_EQ(ins.columns.size(), size_t{2});
  CHECK_EQ(ins.rows.size(), size_t{2});
  CHECK_EQ(std::get<int64_t>(ins.rows[1][1]), int64_t{-2});
}

TEST(parser_select_where_between_limit) {
  auto r = parse_script("SELECT name FROM users WHERE id BETWEEN 3 AND 9 AND name != 'x' LIMIT 5");
  CHECK(r.ok());
  const auto& sel = std::get<SelectStmt>(r.value()[0]);
  CHECK_EQ(sel.where.size(), size_t{3});
  CHECK(sel.where[0].op == CompareOp::kGe);
  CHECK(sel.where[1].op == CompareOp::kLe);
  CHECK(sel.where[2].op == CompareOp::kNe);
  CHECK(sel.limit.has_value() && *sel.limit == 5);
}

TEST(parser_multiple_and_empty_statements) {
  auto r = parse_script(";; SELECT * FROM a; ;SELECT * FROM b");
  CHECK(r.ok());
  CHECK_EQ(r.value().size(), size_t{2});
  auto empty = parse_script("");
  CHECK(empty.ok() && empty.value().empty());
}

TEST(parser_syntax_errors) {
  CHECK_ERR(parse_script("SELEC * FROM t").status(), "expected INSERT or SELECT");
  CHECK_ERR(parse_script("SELECT FROM t").status(), "expected column name");
  CHECK_ERR(parse_script("SELECT * t").status(), "expected FROM");
  CHECK_ERR(parse_script("SELECT * FROM t WHERE").status(), "expected column name");
  CHECK_ERR(parse_script("SELECT * FROM t WHERE a 5").status(), "comparison operator");
  CHECK_ERR(parse_script("SELECT * FROM t LIMIT -1").status(), "LIMIT");
  CHECK_ERR(parse_script("INSERT INTO t VALUES (1").status(), "expected ')'");
  CHECK_ERR(parse_script("INSERT INTO t VALUES (NULL)").status(), "NULL is not supported");
  CHECK_ERR(parse_script("INSERT INTO t VALUES (99999999999999999999)").status(), "out of range");
  CHECK_ERR(parse_script("SELECT * FROM t SELECT * FROM t").status(), "expected ';'");
  CHECK_ERR(parse_script("INSERT INTO t VALUES ('a'").status(), "expected ')'");
}
