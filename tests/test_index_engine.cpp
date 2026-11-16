#include "test_util.h"

using namespace minidb;
using testing::TempDir;

namespace {
std::unique_ptr<Engine> open_engine(const std::string& dir) {
  testing::write_table_schema(dir, "users", testing::kUsersSchema);
  return Engine::open(dir).take();
}

void insert_n(Engine* e, int n) {
  for (int base = 0; base < n; base += 500) {
    std::string sql = "INSERT INTO users (name, age, score) VALUES ";
    for (int i = base; i < std::min(n, base + 500); ++i)
      sql += std::string(i > base ? "," : "") + "('n" + std::to_string(i % 1000) + "'," + std::to_string(i % 90) + ",1.5)";
    CHECK(e->execute(sql).status.ok());
  }
}

QueryResult one(Engine* e, const std::string& sql) {
  ExecOutcome o = e->execute(sql);
  if (!o.status.ok() || o.results.empty()) {
    std::fprintf(stderr, "  query failed: %s -> %s\n", sql.c_str(), o.status.message().c_str());
    ::testing::failure_count()++;
    return {};
  }
  return o.results[0];
}

std::vector<int64_t> ids(const QueryResult& r) {
  std::vector<int64_t> v;
  for (const Row& row : r.rows) v.push_back(std::get<int64_t>(row[0]));
  return v;
}
}  // namespace

TEST(plan_uses_index_for_id_and_scan_otherwise) {
  TempDir dir;
  auto e = open_engine(dir.path());
  insert_n(e.get(), 100);
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE id = 5").plan, std::string("INDEX"));
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE id > 5").plan, std::string("INDEX"));
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE id BETWEEN 5 AND 9").plan, std::string("INDEX"));
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE id = 5 AND age = 5").plan, std::string("INDEX"));
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE age = 5").plan, std::string("SCAN"));
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE name = 'n5'").plan, std::string("SCAN"));
  CHECK_EQ(one(e.get(), "SELECT * FROM users").plan, std::string("SCAN"));
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE id != 5").plan, std::string("SCAN"));
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE id > 5.0").plan, std::string("SCAN"));  // 실수 리터럴은 인덱스 미사용
}

TEST(index_results_equal_scan_results) {
  TempDir dir;
  auto e = open_engine(dir.path());
  insert_n(e.get(), 1500);
  const char* conds[][2] = {
      {"id = 700", "id = 700.0"},
      {"id = 0", "id = 0.0"},
      {"id = 1501", "id = 1501.0"},
      {"id > 1490", "id > 1490.0"},
      {"id >= 1500", "id >= 1500.0"},
      {"id < 4", "id < 4.0"},
      {"id <= 1", "id <= 1.0"},
      {"id BETWEEN 100 AND 130", "id >= 100.0 AND id <= 130.0"},
      {"id > 10 AND id < 5", "id > 10.0 AND id < 5.0"},
      {"id >= 20 AND id <= 20", "id >= 20.0 AND id <= 20.0"},
      {"id > 1000 AND age = 7", "id > 1000.0 AND age = 7"},
      {"id < 300 AND name = 'n7'", "id < 300.0 AND name = 'n7'"},
      {"id > -5", "id > -5.0"},
      {"id < 9223372036854775807", "id < 9223372036854775807.0"},
  };
  for (const auto& c : conds) {
    QueryResult a = one(e.get(), std::string("SELECT * FROM users WHERE ") + c[0]);
    QueryResult b = one(e.get(), std::string("SELECT * FROM users WHERE ") + c[1]);
    CHECK_EQ(a.plan, std::string("INDEX"));
    CHECK_EQ(b.plan, std::string("SCAN"));
    CHECK(ids(a) == ids(b));
    if (ids(a) != ids(b)) std::fprintf(stderr, "    mismatch for: %s\n", c[0]);
  }
}

TEST(index_boundary_literals_do_not_overflow) {
  TempDir dir;
  auto e = open_engine(dir.path());
  insert_n(e.get(), 10);
  CHECK(one(e.get(), "SELECT * FROM users WHERE id > 9223372036854775807").rows.empty());
  CHECK(one(e.get(), "SELECT * FROM users WHERE id < -9223372036854775808").rows.empty());
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE id >= -9223372036854775808").rows.size(), size_t{10});
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE id <= 9223372036854775807").rows.size(), size_t{10});
  CHECK(one(e.get(), "SELECT * FROM users WHERE id = -1").rows.empty());
}

TEST(index_limit_and_order) {
  TempDir dir;
  auto e = open_engine(dir.path());
  insert_n(e.get(), 100);
  QueryResult r = one(e.get(), "SELECT id FROM users WHERE id > 10 LIMIT 3");
  CHECK(ids(r) == std::vector<int64_t>({11, 12, 13}));
  CHECK_EQ(one(e.get(), "SELECT id FROM users WHERE id > 10 LIMIT 0").rows.size(), size_t{0});
  CHECK_EQ(one(e.get(), "SELECT id FROM users WHERE id > 10 LIMIT 0").plan, std::string("INDEX"));
}

TEST(index_is_rebuilt_after_restart) {
  TempDir dir;
  {
    auto e = open_engine(dir.path());
    insert_n(e.get(), 3000);
  }
  auto e = Engine::open(dir.path()).take();
  QueryResult r = one(e.get(), "SELECT * FROM users WHERE id = 2999");
  CHECK_EQ(r.plan, std::string("INDEX"));
  CHECK_EQ(r.rows.size(), size_t{1});
  Table* t = e->get_table("users").value();
  CHECK(t->verify_index().ok());
  CHECK_EQ(t->row_count(), uint64_t{3000});
  // 재시작 후 삽입된 행도 인덱스에 등록된다
  CHECK(e->execute("INSERT INTO users VALUES ('z', 1, 1)").status.ok());
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE id = 3001").rows.size(), size_t{1});
  CHECK(t->verify_index().ok());
}

TEST(index_stays_consistent_after_many_batches_and_failed_inserts) {
  TempDir dir;
  auto e = open_engine(dir.path());
  insert_n(e.get(), 2000);
  CHECK(!e->execute("INSERT INTO users VALUES ('ok', 1, 1), ('bad', 'x', 1)").status.ok());  // 실패한 배치
  insert_n(e.get(), 10);
  Table* t = e->get_table("users").value();
  CHECK(t->verify_index().ok());
  CHECK_EQ(t->row_count(), uint64_t{2010});
  CHECK_EQ(one(e.get(), "SELECT * FROM users WHERE id = 2010").rows.size(), size_t{1});
  CHECK(one(e.get(), "SELECT * FROM users WHERE id = 2011").rows.empty());  // 실패한 INSERT는 id를 소모하지 않음
}

TEST(select_index_hit_returns_projected_columns) {
  TempDir dir;
  auto e = open_engine(dir.path());
  CHECK(e->execute("INSERT INTO users VALUES ('alice', 30, 4.5)").status.ok());
  QueryResult r = one(e.get(), "SELECT name, score FROM users WHERE id = 1");
  CHECK_EQ(r.rows.size(), size_t{1});
  CHECK_EQ(std::get<std::string>(r.rows[0][0]), std::string("alice"));
  CHECK_EQ(std::get<double>(r.rows[0][1]), 4.5);
}
