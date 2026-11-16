#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#include "common/json.h"
#include "server/http_server.h"
#include "test_util.h"

using namespace minidb;
using testing::TempDir;

namespace {

std::unique_ptr<Engine> open_engine(const std::string& dir) {
  testing::write_table_schema(dir, "users", testing::kUsersSchema);
  return Engine::open(dir).take();
}

// ---- 간단한 블로킹 HTTP 클라이언트 ----
int connect_to(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::string read_all(int fd) {
  std::string out;
  char buf[8192];
  while (true) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  return out;
}

struct Response {
  int status = 0;
  std::string body;
};

Response parse_response(const std::string& raw) {
  Response r;
  if (raw.size() < 12 || raw.compare(0, 5, "HTTP/") != 0) return r;
  r.status = std::atoi(raw.c_str() + 9);
  size_t p = raw.find("\r\n\r\n");
  if (p != std::string::npos) r.body = raw.substr(p + 4);
  return r;
}

Response send_raw(uint16_t port, const std::string& request) {
  int fd = connect_to(port);
  if (fd < 0) return {};
  ::send(fd, request.data(), request.size(), 0);
  Response r = parse_response(read_all(fd));
  ::close(fd);
  return r;
}

Response post_query(uint16_t port, const std::string& sql, const char* content_type = "text/plain") {
  return send_raw(port, std::string("POST /query HTTP/1.1\r\nHost: x\r\nContent-Type: ") + content_type +
                            "\r\nContent-Length: " + std::to_string(sql.size()) + "\r\n\r\n" + sql);
}

struct ServerFixture {
  TempDir dir;
  std::unique_ptr<Engine> engine;
  std::unique_ptr<HttpServer> server;
  explicit ServerFixture(ServerOptions opts = ServerOptions()) {
    engine = open_engine(dir.path());
    opts.port = 0;
    server = std::make_unique<HttpServer>(engine.get(), opts);
    Status st = server->start();
    CHECK(st.ok());
  }
  uint16_t port() const { return server->port(); }
};

int64_t first_id_of(const Response& r) {
  auto j = parse_json(r.body);
  if (!j.ok()) return -1;
  const JsonValue* res = j.value().find("results");
  if (!res || res->array.empty()) return -1;
  return static_cast<int64_t>(res->array[0].find("first_id")->number);
}

}  // namespace

// ================= Engine 레벨 동시성 =================

TEST(concurrent_inserts_assign_unique_contiguous_ids) {
  TempDir dir;
  auto e = open_engine(dir.path());
  const int kThreads = 8, kPerThread = 400;
  std::vector<std::vector<int64_t>> got(kThreads);
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t)
    ts.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        // 짝수 번째는 단건, 홀수 번째는 3건 배치
        bool batch = i % 2;
        std::string sql = "INSERT INTO users VALUES ('t" + std::to_string(t) + "', " + std::to_string(i) + ", 1.0)";
        if (batch) sql += ", ('b', 1, 1.0), ('b', 2, 2.0)";
        ExecOutcome o = e->execute(sql);
        if (!o.status.ok()) { CHECK(false); return; }
        for (int64_t id = o.results[0].first_id; id <= o.results[0].last_id; ++id) got[static_cast<size_t>(t)].push_back(id);
      }
    });
  for (auto& th : ts) th.join();
  std::set<int64_t> all;
  size_t total = 0;
  for (auto& v : got) {
    total += v.size();
    all.insert(v.begin(), v.end());
  }
  CHECK_EQ(all.size(), total);                       // id 중복 없음
  CHECK_EQ(*all.begin(), int64_t{1});
  CHECK_EQ(*all.rbegin(), static_cast<int64_t>(total));  // 빈틈 없음
  Table* table = e->get_table("users").value();
  CHECK_EQ(table->row_count(), static_cast<uint64_t>(total));
  CHECK(table->verify_index().ok());
  CHECK_EQ(e->execute("SELECT * FROM users").results[0].rows.size(), total);
}

TEST(concurrent_readers_and_writers_stay_consistent) {
  TempDir dir;
  auto e = open_engine(dir.path());
  CHECK(e->execute("INSERT INTO users VALUES ('seed', 7, 1.0)").status.ok());
  std::atomic<int64_t> committed{1};  // 커밋이 끝난 최대 id (단일 건 INSERT 기준)
  std::atomic<bool> stop{false};
  std::atomic<int> reader_errors{0};

  std::vector<std::thread> writers;
  for (int w = 0; w < 3; ++w)
    writers.emplace_back([&] {
      for (int i = 0; i < 600; ++i) {
        ExecOutcome o = e->execute("INSERT INTO users VALUES ('w', 7, 1.0)");
        if (!o.status.ok()) { reader_errors++; return; }
        int64_t last = o.results[0].last_id;
        int64_t cur = committed.load();
        while (last > cur && !committed.compare_exchange_weak(cur, last)) {}
      }
    });
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r)
    readers.emplace_back([&, r] {
      uint64_t prev_count = 0;
      int i = 0;
      while (!stop) {
        if (r % 2 == 0) {  // 인덱스 조회: 커밋된 id 는 반드시 보여야 한다
          int64_t id = committed.load();
          ExecOutcome o = e->execute("SELECT id, name FROM users WHERE id = " + std::to_string(id));
          if (!o.status.ok() || o.results[0].rows.size() != 1 || o.results[0].plan != "INDEX") reader_errors++;
        } else if (++i % 20 == 0) {  // 선형 스캔: 행 수는 줄어들지 않는다
          ExecOutcome o = e->execute("SELECT id FROM users WHERE age = 7");
          if (!o.status.ok() || o.results[0].rows.size() < prev_count) reader_errors++;
          if (o.status.ok()) prev_count = o.results[0].rows.size();
        }
      }
    });
  for (auto& w : writers) w.join();
  stop = true;
  for (auto& r : readers) r.join();
  CHECK_EQ(reader_errors.load(), 0);
  Table* table = e->get_table("users").value();
  CHECK_EQ(table->row_count(), uint64_t{1 + 3 * 600});
  CHECK(table->verify_index().ok());
}

TEST(concurrent_first_open_of_same_table_is_safe) {
  TempDir dir;
  testing::write_table_schema(dir.path(), "users", testing::kUsersSchema);
  auto e = Engine::open(dir.path()).take();
  std::vector<std::thread> ts;
  std::vector<Table*> seen(8, nullptr);
  for (size_t i = 0; i < 8; ++i)
    ts.emplace_back([&, i] { seen[i] = e->get_table("users").value(); });
  for (auto& t : ts) t.join();
  for (Table* t : seen) CHECK(t == seen[0]);  // 모두 같은 Table 인스턴스
}

// ================= HTTP 서버 =================

TEST(server_health_and_basic_query_flow) {
  ServerFixture f;
  Response h = send_raw(f.port(), "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
  CHECK_EQ(h.status, 200);
  CHECK(h.body.find("\"status\":\"up\"") != std::string::npos);

  Response ins = post_query(f.port(), "INSERT INTO users VALUES ('kim', 30, 1.5), ('lee', 25, 2.5)");
  CHECK_EQ(ins.status, 200);
  CHECK_EQ(first_id_of(ins), int64_t{1});

  Response sel = post_query(f.port(), "SELECT name FROM users WHERE id = 2");
  CHECK_EQ(sel.status, 200);
  auto j = parse_json(sel.body);
  CHECK(j.ok());
  const JsonValue& r0 = j.value().find("results")->array[0];
  CHECK_EQ(r0.find("plan")->string, std::string("INDEX"));
  CHECK_EQ(r0.find("rows")->array[0].array[0].string, std::string("lee"));

  Response js = post_query(f.port(), R"J({"sql":"SELECT * FROM users WHERE age = 30"})J", "application/json");
  CHECK_EQ(js.status, 200);
  CHECK(js.body.find("\"plan\":\"SCAN\"") != std::string::npos);
}

TEST(server_error_status_codes) {
  ServerOptions o;
  o.max_body_bytes = 100;
  ServerFixture f(o);
  CHECK_EQ(post_query(f.port(), "SELEC oops").status, 400);
  CHECK_EQ(post_query(f.port(), "SELECT * FROM missing").status, 404);
  CHECK_EQ(post_query(f.port(), "INSERT INTO users (id, name, age, score) VALUES (1,'a',1,1)").status, 400);
  CHECK_EQ(post_query(f.port(), "").status, 400);
  CHECK_EQ(post_query(f.port(), "{bad json", "application/json").status, 400);
  CHECK_EQ(send_raw(f.port(), "GET /nope HTTP/1.1\r\n\r\n").status, 404);
  CHECK_EQ(send_raw(f.port(), "GET /query HTTP/1.1\r\n\r\n").status, 405);
  CHECK_EQ(send_raw(f.port(), "POST /health HTTP/1.1\r\nContent-Length: 0\r\n\r\n").status, 405);
  CHECK_EQ(send_raw(f.port(), "NONSENSE\r\n\r\n").status, 400);
  CHECK_EQ(send_raw(f.port(), "POST /query HTTP/1.1\r\nContent-Length: abc\r\n\r\n").status, 400);
  CHECK_EQ(send_raw(f.port(), "POST /query HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n").status, 501);
  CHECK_EQ(send_raw(f.port(), "GET /health HTTP/3.0\r\n\r\n").status, 505);
  CHECK_EQ(post_query(f.port(), std::string(500, 'x')).status, 413);  // max_body_bytes=100 초과
  CHECK_EQ(send_raw(f.port(), "GET /health HTTP/1.1\r\nX: " + std::string(20000, 'a') + "\r\n\r\n").status, 431);
  // 실패한 요청 뒤에도 서버는 정상 동작
  CHECK_EQ(send_raw(f.port(), "GET /health HTTP/1.1\r\n\r\n").status, 200);
}

TEST(server_slow_client_gets_408_and_does_not_block_others) {
  ServerOptions o;
  o.threads = 2;
  o.io_timeout_sec = 1;
  ServerFixture f(o);
  int slow = connect_to(f.port());  // 요청을 보내지 않고 대기
  ::send(slow, "POST /query HTTP/1.1\r\n", 22, 0);
  // 다른 워커가 정상 처리한다
  CHECK_EQ(send_raw(f.port(), "GET /health HTTP/1.1\r\n\r\n").status, 200);
  Response r = parse_response(read_all(slow));  // 타임아웃(1s) 후 408
  CHECK_EQ(r.status, 408);
  ::close(slow);
}

TEST(server_returns_503_when_overloaded) {
  ServerOptions o;
  o.threads = 1;
  o.max_queue = 1;
  o.io_timeout_sec = 1;
  ServerFixture f(o);
  int busy = connect_to(f.port());  // 유일한 워커를 점유 (요청 미전송 -> recv 대기)
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  int queued = connect_to(f.port());  // 큐(1)를 채운다
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  Response over = send_raw(f.port(), "GET /health HTTP/1.1\r\n\r\n");  // 큐 포화 -> 503
  CHECK_EQ(over.status, 503);
  CHECK(over.body.find("overloaded") != std::string::npos);
  ::close(busy);
  ::close(queued);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK_EQ(send_raw(f.port(), "GET /health HTTP/1.1\r\n\r\n").status, 200);  // 회복
}

TEST(server_concurrent_clients_get_unique_ids_and_consistent_data) {
  ServerOptions o;
  o.threads = 8;
  ServerFixture f(o);
  const int kClients = 16, kPerClient = 40;
  std::mutex m;
  std::set<int64_t> ids;
  std::atomic<int> failures{0};
  std::vector<std::thread> ts;
  for (int c = 0; c < kClients; ++c)
    ts.emplace_back([&, c] {
      for (int i = 0; i < kPerClient; ++i) {
        Response ins = post_query(f.port(), "INSERT INTO users VALUES ('c" + std::to_string(c) + "', " + std::to_string(i) + ", 1.0)");
        int64_t id = first_id_of(ins);
        if (ins.status != 200 || id < 1) { failures++; continue; }
        {
          std::lock_guard<std::mutex> l(m);
          if (!ids.insert(id).second) failures++;  // 중복 id
        }
        Response sel = post_query(f.port(), "SELECT name, age FROM users WHERE id = " + std::to_string(id));
        auto j = parse_json(sel.body);
        if (sel.status != 200 || !j.ok() || j.value().find("results")->array[0].find("count")->number != 1.0) failures++;
      }
    });
  for (auto& t : ts) t.join();
  CHECK_EQ(failures.load(), 0);
  CHECK_EQ(ids.size(), static_cast<size_t>(kClients * kPerClient));
  Table* table = f.engine->get_table("users").value();
  CHECK_EQ(table->row_count(), static_cast<uint64_t>(kClients * kPerClient));
  CHECK(table->verify_index().ok());
}

TEST(server_graceful_shutdown_finishes_inflight_and_frees_port) {
  TempDir dir;
  auto e = open_engine(dir.path());
  ServerOptions o;
  o.port = 0;
  HttpServer server(e.get(), o);
  CHECK(server.start().ok());
  uint16_t port = server.port();
  CHECK_EQ(post_query(port, "INSERT INTO users VALUES ('a', 1, 1.0)").status, 200);
  server.stop();
  server.stop();  // 중복 호출 안전
  CHECK(connect_to(port) < 0);  // 더 이상 연결을 받지 않음
  // 같은 데이터 디렉터리로 재시작해도 데이터가 남아 있다
  HttpServer again(e.get(), o);
  CHECK(again.start().ok());
  Response r = post_query(again.port(), "SELECT * FROM users");
  CHECK(r.body.find("\"count\":1") != std::string::npos);
}

TEST(server_start_fails_on_bad_address_and_port_in_use) {
  TempDir dir;
  auto e = open_engine(dir.path());
  ServerOptions bad;
  bad.host = "not-an-ip";
  bad.port = 0;
  HttpServer s1(e.get(), bad);
  CHECK_ERR(s1.start(), "invalid host");
  ServerOptions ok;
  ok.port = 0;
  HttpServer s2(e.get(), ok);
  CHECK(s2.start().ok());
  ServerOptions same;
  same.port = s2.port();
  HttpServer s3(e.get(), same);
  CHECK_ERR(s3.start(), "cannot listen");
}
