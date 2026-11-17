// 벤치마크: 100만 건 삽입(SQL 경로 / 벌크 경로) 후 ID 인덱스 조회 vs 비인덱스 선형 탐색을 측정하고
// docs/benchmark.md, docs/benchmark.svg, bench/results.csv 를 생성한다. (모든 수치는 이 실행의 실측값)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#include "engine/engine.h"

using namespace minidb;
using Clock = std::chrono::steady_clock;

namespace {

struct Stats {
  std::string name;
  std::string plan;
  size_t n = 0;
  double mean_us = 0, p50_us = 0, p99_us = 0, max_us = 0;
};

double elapsed_us(Clock::time_point a) {
  return std::chrono::duration<double, std::micro>(Clock::now() - a).count();
}

Stats summarize(const std::string& name, const std::string& plan, std::vector<double> us) {
  Stats s;
  s.name = name;
  s.plan = plan;
  s.n = us.size();
  std::sort(us.begin(), us.end());
  double sum = 0;
  for (double v : us) sum += v;
  s.mean_us = sum / static_cast<double>(us.size());
  s.p50_us = us[us.size() / 2];
  s.p99_us = us[std::min(us.size() - 1, static_cast<size_t>(static_cast<double>(us.size()) * 0.99))];
  s.max_us = us.back();
  return s;
}

[[noreturn]] void die(const std::string& m) {
  std::fprintf(stderr, "bench error: %s\n", m.c_str());
  std::exit(1);
}

QueryResult run_one(Engine* e, const std::string& sql) {
  ExecOutcome o = e->execute(sql);
  if (!o.status.ok() || o.results.empty()) die(sql + " -> " + o.status.message());
  return o.results[0];
}

std::string fmt_time(double us) {
  char buf[48];
  if (us >= 1000.0) std::snprintf(buf, sizeof(buf), "%.2f ms", us / 1000.0);
  else std::snprintf(buf, sizeof(buf), "%.2f µs", us);
  return buf;
}

const char* kSchema =
    R"J({"table":"T","columns":[{"name":"id","type":"BIGINT"},{"name":"name","type":"VARCHAR(32)"},)J"
    R"J({"name":"age","type":"INT"},{"name":"email","type":"VARCHAR(64)"}]})J";

void make_table(const std::string& dir, const std::string& name) {
  std::filesystem::create_directories(dir + "/" + name);
  std::string schema = kSchema;
  schema.replace(schema.find("\"T\""), 3, "\"" + name + "\"");
  std::ofstream(dir + "/" + name + "/schema.json") << schema;
}

std::string name_of(int64_t i) { return "user" + std::to_string(i); }
std::string email_of(int64_t i) { return "user" + std::to_string(i) + "@bench.com"; }

// 로그 스케일 막대 그래프 (평균 지연시간 µs)
void write_svg(const std::string& path, const std::vector<Stats>& stats) {
  const int w = 760, row_h = 46, top = 50, left = 250;
  int h = top + static_cast<int>(stats.size()) * row_h + 40;
  double max_log = 0;
  for (const Stats& s : stats) max_log = std::max(max_log, std::log10(std::max(1.0, s.mean_us)));
  max_log = std::ceil(max_log + 0.2);
  std::ostringstream o;
  o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h << "\" font-family=\"sans-serif\" font-size=\"13\">\n";
  o << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";
  o << "<text x=\"" << left << "\" y=\"26\" font-weight=\"bold\">Mean query latency (log scale, lower is better)</text>\n";
  // 눈금선: 10의 거듭제곱(1µs, 10µs, ... ) 마다 세로선과 라벨. 막대 길이 = log10(µs) 에 비례.
  const double axis_w = w - left - 150;
  const int axis_bottom = top + static_cast<int>(stats.size()) * row_h;
  for (int d = 0; d <= static_cast<int>(max_log); ++d) {
    double x = left + d / max_log * axis_w;
    o << "<line x1=\"" << x << "\" y1=\"" << top - 4 << "\" x2=\"" << x << "\" y2=\"" << axis_bottom
      << "\" stroke=\"#cccccc\" stroke-dasharray=\"3,3\"/>\n";
    o << "<text x=\"" << x << "\" y=\"" << axis_bottom + 16 << "\" text-anchor=\"middle\" font-size=\"11\" fill=\"#666666\">"
      << fmt_time(std::pow(10.0, d)) << "</text>\n";
  }
  for (size_t i = 0; i < stats.size(); ++i) {
    const Stats& s = stats[i];
    int y = top + static_cast<int>(i) * row_h;
    double len = std::log10(std::max(1.0, s.mean_us)) / max_log * axis_w;
    const char* color = s.plan == "INDEX" ? "#2a7de1" : "#e1572a";
    o << "<text x=\"" << left - 10 << "\" y=\"" << y + 20 << "\" text-anchor=\"end\">" << s.name << "</text>\n";
    o << "<rect x=\"" << left << "\" y=\"" << y + 4 << "\" width=\"" << std::max(2.0, len) << "\" height=\"26\" fill=\"" << color << "\"/>\n";
    o << "<text x=\"" << left + std::max(2.0, len) + 8 << "\" y=\"" << y + 22 << "\">" << fmt_time(s.mean_us) << " (" << s.plan << ")</text>\n";
  }
  o << "</svg>\n";
  std::ofstream(path) << o.str();
}

}  // namespace

int main(int argc, char** argv) {
  int64_t rows = 1000000;
  int index_queries = 10000, scan_queries = 20, range_queries = 1000;
  std::string data_dir = "build/bench_data";
  bool write_reports = true;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--rows" && i + 1 < argc) rows = std::atoll(argv[++i]);
    else if (a == "--scan-queries" && i + 1 < argc) scan_queries = std::atoi(argv[++i]);
    else if (a == "--no-report") write_reports = false;
    else die("usage: bench [--rows N] [--scan-queries N] [--no-report]");
  }
  if (rows < 1000) die("--rows must be >= 1000");

  // 벤치마크 전용 디렉터리만 정리한다 (CLAUDE.md 절대 규칙 5: data/ 는 건드리지 않음).
  std::filesystem::remove_all(data_dir);
  std::filesystem::create_directories(data_dir);
  make_table(data_dir, "bench_sql");
  make_table(data_dir, "bench_bulk");
  auto engine = Engine::open(data_dir);
  if (!engine.ok()) die(engine.status().message());
  Engine* e = engine.value().get();

  // ---- 1) SQL 경로 삽입: 1000행짜리 INSERT 문을 반복 실행 ----
  std::printf("[1/4] inserting %lld rows through SQL (batches of 1000)...\n", static_cast<long long>(rows));
  auto t0 = Clock::now();
  for (int64_t base = 0; base < rows; base += 1000) {
    std::string sql = "INSERT INTO bench_sql (name, age, email) VALUES ";
    int64_t end = std::min(rows, base + 1000);
    for (int64_t i = base; i < end; ++i) {
      if (i > base) sql += ',';
      sql += "('" + name_of(i + 1) + "'," + std::to_string(i % 100) + ",'" + email_of(i + 1) + "')";
    }
    ExecOutcome o = e->execute(sql);
    if (!o.status.ok()) die(o.status.message());
  }
  double sql_insert_s = elapsed_us(t0) / 1e6;

  // ---- 2) 벌크 경로 삽입: SQL 파싱을 건너뛰고 Table::insert 로 직접 (같은 락/인덱스 경로) ----
  std::printf("[2/4] inserting %lld rows through the bulk path (batches of 10000)...\n", static_cast<long long>(rows));
  Table* bulk = e->get_table("bench_bulk").value();
  t0 = Clock::now();
  for (int64_t base = 0; base < rows; base += 10000) {
    std::vector<Row> batch;
    int64_t end = std::min(rows, base + 10000);
    batch.reserve(static_cast<size_t>(end - base));
    for (int64_t i = base; i < end; ++i)
      batch.push_back(Row{int64_t{0}, name_of(i + 1), static_cast<int64_t>(i % 100), email_of(i + 1)});
    if (!bulk->insert(std::move(batch)).ok()) die("bulk insert failed");
  }
  double bulk_insert_s = elapsed_us(t0) / 1e6;

  Table* sqlt = e->get_table("bench_sql").value();
  if (sqlt->row_count() != static_cast<uint64_t>(rows) || bulk->row_count() != static_cast<uint64_t>(rows)) die("row count mismatch");
  Status vs = sqlt->verify_index();
  if (!vs.ok()) die("index verification failed: " + vs.message());
  vs = bulk->verify_index();
  if (!vs.ok()) die("index verification failed: " + vs.message());

  // ---- 3) 조회 성능 ----
  std::printf("[3/4] measuring queries...\n");
  std::mt19937_64 rng(20240601);
  auto rand_id = [&] { return static_cast<int64_t>(rng() % static_cast<uint64_t>(rows)) + 1; };
  std::vector<Stats> stats;

  { // 인덱스 점 조회
    std::vector<double> us;
    for (int i = 0; i < index_queries; ++i) {
      std::string q = "SELECT * FROM bench_sql WHERE id = " + std::to_string(rand_id());
      auto t = Clock::now();
      QueryResult r = run_one(e, q);
      us.push_back(elapsed_us(t));
      if (r.plan != "INDEX" || r.rows.size() != 1) die("expected INDEX plan with one row");
    }
    stats.push_back(summarize("WHERE id = ? (point)", "INDEX", us));
  }
  { // 인덱스 범위 조회 (1000건)
    std::vector<double> us;
    for (int i = 0; i < range_queries; ++i) {
      int64_t lo = rand_id() % (rows - 1000) + 1;
      std::string q = "SELECT * FROM bench_sql WHERE id BETWEEN " + std::to_string(lo) + " AND " + std::to_string(lo + 999);
      auto t = Clock::now();
      QueryResult r = run_one(e, q);
      us.push_back(elapsed_us(t));
      if (r.plan != "INDEX" || r.rows.size() != 1000) die("expected INDEX range of 1000 rows");
    }
    stats.push_back(summarize("WHERE id BETWEEN (1000 rows)", "INDEX", us));
  }
  { // 비인덱스 필드 점 조회: 선형 탐색 (email 은 행마다 유일하므로 결과는 1건)
    std::vector<double> us;
    for (int i = 0; i < scan_queries; ++i) {
      std::string q = "SELECT * FROM bench_sql WHERE email = '" + email_of(rand_id()) + "'";
      auto t = Clock::now();
      QueryResult r = run_one(e, q);
      us.push_back(elapsed_us(t));
      if (r.plan != "SCAN" || r.rows.size() != 1) die("expected SCAN plan with one row");
    }
    stats.push_back(summarize("WHERE email = ? (point)", "SCAN", us));
  }
  { // 비인덱스 필드 조회: age (1% 선택도, 약 1만 건 반환)
    std::vector<double> us;
    for (int i = 0; i < std::max(3, scan_queries / 4); ++i) {
      std::string q = "SELECT * FROM bench_sql WHERE age = " + std::to_string(rng() % 100);
      auto t = Clock::now();
      QueryResult r = run_one(e, q);
      us.push_back(elapsed_us(t));
      if (r.plan != "SCAN") die("expected SCAN plan");
    }
    stats.push_back(summarize("WHERE age = ? (~1% rows)", "SCAN", us));
  }

  // ---- 정합성: 같은 행을 인덱스(id)와 선형 탐색(email)으로 가져와 동일한지 확인 ----
  for (int i = 0; i < 20; ++i) {
    int64_t id = rand_id();
    QueryResult a = run_one(e, "SELECT * FROM bench_sql WHERE id = " + std::to_string(id));
    QueryResult b = run_one(e, "SELECT * FROM bench_sql WHERE email = '" + email_of(id) + "'");
    if (a.rows != b.rows) die("index and scan results differ for id " + std::to_string(id));
  }

  double speedup = stats[2].mean_us / stats[0].mean_us;

  // ---- 4) 리포트 ----
  std::printf("[4/4] results\n");
  std::printf("  SQL insert : %.2f s (%.0f rows/s)\n", sql_insert_s, static_cast<double>(rows) / sql_insert_s);
  std::printf("  bulk insert: %.2f s (%.0f rows/s)\n", bulk_insert_s, static_cast<double>(rows) / bulk_insert_s);
  for (const Stats& s : stats)
    std::printf("  %-32s %-5s n=%-6zu mean=%s p50=%s p99=%s\n", s.name.c_str(), s.plan.c_str(), s.n, fmt_time(s.mean_us).c_str(),
                fmt_time(s.p50_us).c_str(), fmt_time(s.p99_us).c_str());
  std::printf("  speedup (email scan mean / id index mean): %.0fx\n", speedup);

  if (write_reports) {
    std::filesystem::create_directories("docs");
    std::time_t now = std::time(nullptr);
    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", std::localtime(&now));
    std::ostringstream md;
    md << "# Benchmark Report\n\n";
    md << "> 이 파일은 `make bench` 가 자동 생성합니다. 모든 수치는 해당 실행의 실측값입니다.\n\n";
    md << "- 실행 시각: " << date << "\n- 레코드 수: " << rows << " (테이블당)\n- 하드웨어 스레드: "
       << std::thread::hardware_concurrency() << "\n- 스키마: id BIGINT, name VARCHAR(32), age INT, email VARCHAR(64)\n";
    md << "- B+ 트리: 노드당 최대 128 키, 키=id, 값=data.bin offset\n- 데이터 파일 크기: "
       << std::filesystem::file_size(data_dir + "/bench_sql/data.bin") / (1024 * 1024) << " MiB\n\n";
    char buf[160];
    md << "## 삽입 (" << rows << " rows)\n\n| 경로 | 소요 시간 | 처리량 |\n|---|---|---|\n";
    std::snprintf(buf, sizeof(buf), "| SQL (1000행 INSERT 문 반복) | %.2f s | %.0f rows/s |\n", sql_insert_s, static_cast<double>(rows) / sql_insert_s);
    md << buf;
    std::snprintf(buf, sizeof(buf), "| 벌크 (Table::insert, 10000행 배치) | %.2f s | %.0f rows/s |\n\n", bulk_insert_s, static_cast<double>(rows) / bulk_insert_s);
    md << buf;
    md << "## 조회\n\n| 쿼리 | 실행 계획 | 횟수 | 평균 | p50 | p99 | 최대 |\n|---|---|---|---|---|---|---|\n";
    for (const Stats& s : stats)
      md << "| `" << s.name << "` | " << s.plan << " | " << s.n << " | " << fmt_time(s.mean_us) << " | " << fmt_time(s.p50_us) << " | "
         << fmt_time(s.p99_us) << " | " << fmt_time(s.max_us) << " |\n";
    std::snprintf(buf, sizeof(buf), "\n**ID 인덱스 점 조회는 비인덱스(email) 선형 탐색보다 평균 약 %.0f배 빠르다.**\n\n", speedup);
    md << buf;
    md << "![latency](benchmark.svg)\n\n";
    md << "## 측정 방법\n\n- 모든 쿼리는 `Engine::execute()`(파싱 포함)로 실행하며 지연시간은 파싱+실행+결과 구성을 포함한다.\n"
          "- 점 조회 대상 id/email 은 고정 시드 난수로 선택한다. 각 쿼리는 결과 행 수와 실행 계획(INDEX/SCAN)을 검증한다.\n"
          "- 같은 행을 id(인덱스)와 email(선형 탐색)로 각각 조회해 결과가 동일한지 20회 교차 검증했다.\n"
          "- 삽입 후 `Table::verify_index()` 로 B+ 트리 불변식과 힙 파일 offset 일치를 검증했다.\n"
          "- 캐시: OS 페이지 캐시에 올라간 상태(warm)에서의 측정이다. 인덱스는 메모리 상주.\n"
          "- 재현: `make bench` (행 수 변경: `./build/bench --rows N`).\n";
    std::ofstream("docs/benchmark.md") << md.str();
    write_svg("docs/benchmark.svg", stats);
    std::ofstream csv("bench/results.csv");
    csv << "name,plan,queries,mean_us,p50_us,p99_us,max_us\n";
    for (const Stats& s : stats)
      csv << '"' << s.name << "\"," << s.plan << ',' << s.n << ',' << s.mean_us << ',' << s.p50_us << ',' << s.p99_us << ',' << s.max_us << '\n';
    csv << "insert_sql_seconds,,," << sql_insert_s << ",,,\ninsert_bulk_seconds,,," << bulk_insert_s << ",,,\n";
    std::printf("reports written: docs/benchmark.md, docs/benchmark.svg, bench/results.csv\n");
  }
  std::filesystem::remove_all(data_dir);
  return 0;
}
