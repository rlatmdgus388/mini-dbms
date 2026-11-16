#include <fcntl.h>
#include <unistd.h>

#include "test_util.h"

using namespace minidb;
using testing::TempDir;

namespace {
std::unique_ptr<Engine> open_engine(const std::string& dir) {
  testing::write_table_schema(dir, "users", testing::kUsersSchema);
  return Engine::open(dir).take();
}
}  // namespace

TEST(engine_insert_select_roundtrip) {
  TempDir dir;
  auto e = open_engine(dir.path());
  auto out = e->execute("INSERT INTO users (name, age, score) VALUES ('alice', 30, 1.5), ('bob', 25, 2);"
                        "SELECT * FROM users;");
  CHECK(out.status.ok());
  CHECK_EQ(out.results.size(), size_t{2});
  CHECK_EQ(out.results[0].affected, uint64_t{2});
  CHECK_EQ(out.results[0].first_id, int64_t{1});
  CHECK_EQ(out.results[0].last_id, int64_t{2});
  const auto& sel = out.results[1];
  CHECK_EQ(sel.rows.size(), size_t{2});
  CHECK_EQ(std::get<int64_t>(sel.rows[1][0]), int64_t{2});
  CHECK_EQ(std::get<std::string>(sel.rows[0][1]), std::string("alice"));
  CHECK_EQ(std::get<double>(sel.rows[1][3]), 2.0);  // 정수 리터럴이 DOUBLE 로 변환됨
  CHECK_EQ(sel.columns.size(), size_t{4});
}

TEST(engine_insert_without_column_list_uses_schema_order) {
  TempDir dir;
  auto e = open_engine(dir.path());
  CHECK(e->execute("INSERT INTO users VALUES ('x', 1, 0.5)").status.ok());
  auto out = e->execute("SELECT name, age FROM users");
  CHECK(out.status.ok());
  CHECK_EQ(out.results[0].columns[1], std::string("age"));
  CHECK_EQ(std::get<int64_t>(out.results[0].rows[0][1]), int64_t{1});
}

TEST(engine_where_filters_and_limit) {
  TempDir dir;
  auto e = open_engine(dir.path());
  std::string ins = "INSERT INTO users (name, age, score) VALUES ";
  for (int i = 1; i <= 20; ++i) ins += std::string(i > 1 ? "," : "") + "('u" + std::to_string(i) + "', " + std::to_string(i) + ", 0.5)";
  CHECK(e->execute(ins).status.ok());
  auto count = [&](const std::string& q) {
    auto o = e->execute(q);
    return o.status.ok() ? static_cast<int>(o.results[0].rows.size()) : -1;
  };
  CHECK_EQ(count("SELECT * FROM users WHERE age = 5"), 1);
  CHECK_EQ(count("SELECT * FROM users WHERE age > 15"), 5);
  CHECK_EQ(count("SELECT * FROM users WHERE age >= 15 AND age < 18"), 3);
  CHECK_EQ(count("SELECT * FROM users WHERE age != 5"), 19);
  CHECK_EQ(count("SELECT * FROM users WHERE age BETWEEN 3 AND 6"), 4);
  CHECK_EQ(count("SELECT * FROM users WHERE name = 'u7'"), 1);
  CHECK_EQ(count("SELECT * FROM users WHERE age > 2.5 AND age < 4.5"), 2);  // 정수 컬럼 vs 실수 리터럴
  CHECK_EQ(count("SELECT * FROM users WHERE name = 'zzz'"), 0);
  CHECK_EQ(count("SELECT * FROM users LIMIT 3"), 3);
  CHECK_EQ(count("SELECT * FROM users LIMIT 0"), 0);
  CHECK_EQ(count("SELECT * FROM users WHERE age > 10 LIMIT 2"), 2);
  CHECK_EQ(count("SELECT * FROM users LIMIT 1000"), 20);
}

TEST(engine_rejects_user_specified_id) {
  TempDir dir;
  auto e = open_engine(dir.path());
  auto out = e->execute("INSERT INTO users (id, name, age, score) VALUES (5, 'a', 1, 1)");
  CHECK_ERR(out.status, "auto-assigned");
  CHECK(e->execute("SELECT * FROM users").results[0].rows.empty());
}

TEST(engine_insert_validation_errors) {
  TempDir dir;
  auto e = open_engine(dir.path());
  CHECK_ERR(e->execute("INSERT INTO nope VALUES (1)").status, "does not exist");
  CHECK_ERR(e->execute("INSERT INTO users (name) VALUES ('a')").status, "all columns must be specified");
  CHECK_ERR(e->execute("INSERT INTO users (name, age, bogus) VALUES ('a', 1, 2)").status, "unknown column");
  CHECK_ERR(e->execute("INSERT INTO users (name, name, age) VALUES ('a', 'b', 1)").status, "duplicate column");
  CHECK_ERR(e->execute("INSERT INTO users VALUES ('a', 1)").status, "expected 3 values");
  CHECK_ERR(e->execute("INSERT INTO users VALUES (1, 1, 1)").status, "expects a string");
  CHECK_ERR(e->execute("INSERT INTO users VALUES ('a', 'b', 1)").status, "expects an integer");
  CHECK_ERR(e->execute("INSERT INTO users VALUES ('a', 1.5, 1)").status, "expects an integer");
  CHECK_ERR(e->execute("INSERT INTO users VALUES ('a', 1, 'x')").status, "expects a number");
  CHECK_ERR(e->execute("INSERT INTO users VALUES ('a', 2147483648, 1)").status, "out of range for INT");
  CHECK_ERR(e->execute("INSERT INTO users VALUES ('12345678901', 1, 1)").status, "too long");
  CHECK(e->execute("INSERT INTO users VALUES ('1234567890', -2147483648, 1)").status.ok());  // 경계값
  // 배치 중간 실패 시 아무 행도 들어가지 않는다 (원자성)
  CHECK(!e->execute("INSERT INTO users VALUES ('ok', 1, 1), ('bad', 'x', 1)").status.ok());
  CHECK_EQ(e->execute("SELECT * FROM users").results[0].rows.size(), size_t{1});
}

TEST(engine_select_validation_errors) {
  TempDir dir;
  auto e = open_engine(dir.path());
  CHECK_ERR(e->execute("SELECT * FROM nope").status, "does not exist");
  CHECK_ERR(e->execute("SELECT bogus FROM users").status, "unknown column");
  CHECK_ERR(e->execute("SELECT * FROM users WHERE bogus = 1").status, "unknown column");
  CHECK_ERR(e->execute("SELECT * FROM users WHERE name = 5").status, "cannot compare");
  CHECK_ERR(e->execute("SELECT * FROM users WHERE age = 'x'").status, "cannot compare");
  CHECK_ERR(e->execute("SELECT * FROM users WHERE age =").status, "literal");
}

TEST(engine_table_name_path_traversal_rejected) {
  TempDir dir;
  auto e = open_engine(dir.path());
  CHECK_ERR(e->execute("SELECT * FROM users/../x").status, "syntax error");
  CHECK_ERR(e->get_table("../users").status(), "invalid table name");
  CHECK_ERR(e->get_table("a/b").status(), "invalid table name");
}

TEST(engine_stops_at_first_failing_statement_but_keeps_prior_results) {
  TempDir dir;
  auto e = open_engine(dir.path());
  auto out = e->execute("INSERT INTO users VALUES ('a', 1, 1); SELECT * FROM nope; INSERT INTO users VALUES ('b', 2, 2)");
  CHECK(!out.status.ok());
  CHECK_EQ(out.results.size(), size_t{1});
  CHECK_EQ(e->execute("SELECT * FROM users").results[0].rows.size(), size_t{1});
}

TEST(engine_syntax_error_executes_nothing) {
  TempDir dir;
  auto e = open_engine(dir.path());
  auto out = e->execute("INSERT INTO users VALUES ('a', 1, 1); SELEC oops");
  CHECK(!out.status.ok());
  CHECK(out.results.empty());
  CHECK(e->execute("SELECT * FROM users").results[0].rows.empty());
}

TEST(engine_empty_input_is_noop) {
  TempDir dir;
  auto e = open_engine(dir.path());
  auto out = e->execute("  -- just a comment\n ;");
  CHECK(out.status.ok());
  CHECK(out.results.empty());
}

TEST(engine_persists_across_restart_and_ids_continue) {
  TempDir dir;
  {
    auto e = open_engine(dir.path());
    CHECK(e->execute("INSERT INTO users VALUES ('a', 1, 1), ('b', 2, 2)").status.ok());
  }
  auto e = Engine::open(dir.path()).take();
  auto out = e->execute("INSERT INTO users VALUES ('c', 3, 3); SELECT name FROM users");
  CHECK(out.status.ok());
  CHECK_EQ(out.results[0].first_id, int64_t{3});
  CHECK_EQ(out.results[1].rows.size(), size_t{3});
  CHECK_EQ(std::get<std::string>(out.results[1].rows[2][0]), std::string("c"));
}

TEST(engine_recovers_from_torn_tail_on_restart) {
  TempDir dir;
  {
    auto e = open_engine(dir.path());
    CHECK(e->execute("INSERT INTO users VALUES ('a', 1, 1)").status.ok());
  }
  int fd = ::open((dir.path() + "/users/data.bin").c_str(), O_WRONLY | O_APPEND);
  uint32_t n = 500;
  CHECK(::write(fd, &n, 4) == 4);
  CHECK(::write(fd, "xx", 2) == 2);
  ::close(fd);
  auto e = Engine::open(dir.path()).take();
  CHECK_EQ(e->execute("SELECT * FROM users").results[0].rows.size(), size_t{1});
  auto out = e->execute("INSERT INTO users VALUES ('b', 2, 2); SELECT * FROM users");
  CHECK(out.status.ok());
  CHECK_EQ(out.results[0].first_id, int64_t{2});
  CHECK_EQ(out.results[1].rows.size(), size_t{2});
}

TEST(engine_unicode_and_quotes_roundtrip) {
  TempDir dir;
  auto e = open_engine(dir.path());
  CHECK(e->execute("INSERT INTO users VALUES ('it''s', 1, 1), ('한글', 2, 2)").status.ok());
  auto out = e->execute("SELECT name FROM users WHERE name = 'it''s'");
  CHECK_EQ(out.results[0].rows.size(), size_t{1});
  out = e->execute("SELECT name FROM users WHERE name = '한글'");
  CHECK_EQ(out.results[0].rows.size(), size_t{1});
}

TEST(engine_open_missing_data_dir_fails) {
  CHECK_ERR(Engine::open("build/test_tmp/definitely_missing_dir").status(), "does not exist");
}

TEST(engine_corrupt_schema_reports_error) {
  TempDir dir;
  testing::write_table_schema(dir.path(), "bad", "{ not json");
  auto e = Engine::open(dir.path()).take();
  CHECK_ERR(e->execute("SELECT * FROM bad").status, "invalid schema");
}
