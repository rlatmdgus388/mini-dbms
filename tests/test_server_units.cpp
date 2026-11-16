#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>

#include "common/json.h"
#include "server/api.h"
#include "server/http.h"
#include "server/thread_pool.h"
#include "test_util.h"

using namespace minidb;

TEST(threadpool_runs_all_tasks_in_parallel_workers) {
  ThreadPool pool(4, 1000);
  std::atomic<int> sum{0};
  std::mutex m;
  std::set<std::thread::id> ids;
  for (int i = 0; i < 400; ++i)
    CHECK(pool.submit([&] {
      sum++;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      std::lock_guard<std::mutex> l(m);
      ids.insert(std::this_thread::get_id());
    }));
  pool.shutdown();  // graceful: 큐를 모두 비운 뒤 반환
  CHECK_EQ(sum.load(), 400);
  CHECK(ids.size() > 1);  // 실제로 여러 워커 스레드가 사용됨
  CHECK(ids.size() <= 4);
}

TEST(threadpool_rejects_when_queue_full_or_stopped) {
  ThreadPool pool(1, 2);
  std::atomic<bool> release{false};
  std::atomic<bool> started{false};
  CHECK(pool.submit([&] {
    started = true;
    while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }));
  while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(pool.submit([] {}));
  CHECK(pool.submit([] {}));
  CHECK(!pool.submit([] {}));  // 큐(2) 가득 참
  CHECK_EQ(pool.pending(), size_t{2});
  release = true;
  pool.shutdown();
  CHECK(!pool.submit([] {}));  // 종료 후 거부
  pool.shutdown();             // 중복 호출 안전
}

TEST(threadpool_survives_throwing_task) {
  ThreadPool pool(1, 10);
  std::atomic<int> ran{0};
  CHECK(pool.submit([] { throw 42; }));
  CHECK(pool.submit([&] { ran++; }));
  pool.shutdown();
  CHECK_EQ(ran.load(), 1);
}

TEST(http_parse_valid_request) {
  HttpRequest req;
  size_t len = 0;
  std::string err;
  int rc = parse_request_head("POST /query?x=1 HTTP/1.1\r\nHost: a\r\nContent-Length:  12 \r\nContent-Type: text/plain", &req, &len, &err);
  CHECK_EQ(rc, 0);
  CHECK_EQ(req.method, std::string("POST"));
  CHECK_EQ(req.path, std::string("/query"));
  CHECK_EQ(len, size_t{12});
  CHECK_EQ(req.headers["content-type"], std::string("text/plain"));
}

TEST(http_parse_rejects_malformed) {
  HttpRequest req;
  size_t len = 0;
  std::string err;
  CHECK_EQ(parse_request_head("GARBAGE", &req, &len, &err), 400);
  CHECK_EQ(parse_request_head("GET / HTTP/1.1 extra", &req, &len, &err), 400);
  CHECK_EQ(parse_request_head("GET noslash HTTP/1.1", &req, &len, &err), 400);
  CHECK_EQ(parse_request_head("get / HTTP/1.1", &req, &len, &err), 400);
  CHECK_EQ(parse_request_head("GET / HTTP/2.0", &req, &len, &err), 505);
  CHECK_EQ(parse_request_head("POST / HTTP/1.1\r\nContent-Length: abc", &req, &len, &err), 400);
  CHECK_EQ(parse_request_head("POST / HTTP/1.1\r\nContent-Length: -5", &req, &len, &err), 400);
  CHECK_EQ(parse_request_head("POST / HTTP/1.1\r\nContent-Length: 99999999999999999999", &req, &len, &err), 400);
  CHECK_EQ(parse_request_head("POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6", &req, &len, &err), 400);
  CHECK_EQ(parse_request_head("POST / HTTP/1.1\r\nTransfer-Encoding: chunked", &req, &len, &err), 501);
  CHECK_EQ(parse_request_head("GET / HTTP/1.1\r\nno-colon-header", &req, &len, &err), 400);
  CHECK_EQ(parse_request_head("POST / HTTP/1.1\r\nX: a\nContent-Length: 3", &req, &len, &err), 400);  // bare LF
  CHECK_EQ(parse_request_head("POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5", &req, &len, &err), 0);  // 동일 값 중복은 허용
}

TEST(api_extract_sql_variants) {
  CHECK_EQ(extract_sql("text/plain", "SELECT 1").value(), std::string("SELECT 1"));
  CHECK_EQ(extract_sql("application/json", R"J({"sql":"SELECT * FROM t"})J").value(), std::string("SELECT * FROM t"));
  CHECK_EQ(extract_sql("", R"J(  {"sql":"x"})J").value(), std::string("x"));  // 타입 없이 JSON 처럼 보이면 JSON
  CHECK(!extract_sql("application/json", "{bad").ok());
  CHECK(!extract_sql("application/json", R"J({"sql":5})J").ok());
  CHECK(!extract_sql("application/json", "{}").ok());
  CHECK(!extract_sql("text/plain", "   \n").ok());
  CHECK(!extract_sql("text/plain", "").ok());
}

TEST(api_outcome_json_shape_and_escaping) {
  ExecOutcome o;
  QueryResult sel;
  sel.kind = "SELECT";
  sel.columns = {"id", "name", "score"};
  sel.rows = {Row{int64_t{1}, std::string("a\"b\n"), 2.5}};
  sel.plan = "INDEX";
  o.results.push_back(sel);
  QueryResult ins;
  ins.kind = "INSERT";
  ins.affected = 2;
  ins.first_id = 5;
  ins.last_id = 6;
  o.results.push_back(ins);
  std::string json = outcome_to_json(o, 1.5);
  auto parsed = parse_json(json);  // 생성한 JSON 이 유효해야 한다
  CHECK(parsed.ok());
  if (parsed.ok()) {
    CHECK(parsed.value().find("ok")->boolean);
    const JsonValue* results = parsed.value().find("results");
    CHECK_EQ(results->array.size(), size_t{2});
    CHECK_EQ(results->array[0].find("rows")->array[0].array[1].string, std::string("a\"b\n"));
    CHECK_EQ(results->array[0].find("plan")->string, std::string("INDEX"));
    CHECK_EQ(results->array[1].find("last_id")->number, 6.0);
  }
  o.status = Status::fail(ErrorCode::kInvalid, "bad \"thing\"");
  auto failed = parse_json(outcome_to_json(o, 0));
  CHECK(failed.ok() && !failed.value().find("ok")->boolean);
  CHECK_EQ(failed.value().find("error")->string, std::string("bad \"thing\""));
}

TEST(api_status_mapping) {
  CHECK_EQ(http_status_for(ErrorCode::kParse), 400);
  CHECK_EQ(http_status_for(ErrorCode::kInvalid), 400);
  CHECK_EQ(http_status_for(ErrorCode::kNotFound), 404);
  CHECK_EQ(http_status_for(ErrorCode::kIo), 500);
  CHECK_EQ(http_status_for(ErrorCode::kInternal), 500);
}
