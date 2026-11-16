#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "common/result.h"
#include "engine/engine.h"
#include "server/thread_pool.h"

namespace minidb {

struct ServerOptions {
  std::string host = "127.0.0.1";
  uint16_t port = 8080;       // 0 이면 OS가 빈 포트를 고른다 (port()로 확인)
  size_t threads = 8;         // 워커 수
  size_t max_queue = 256;     // 대기 연결 수. 초과하면 503
  size_t max_body_bytes = 1024 * 1024;
  int io_timeout_sec = 5;     // 읽기/쓰기 타임아웃 (느린 클라이언트가 워커를 점유하지 못하게)
};

// 구조: acceptor 스레드 1개가 연결을 accept 해 ThreadPool 작업 큐에 넣고, 워커가 요청을 파싱해
// Engine::execute() 로 실행한다. 동시성은 Table 의 shared_mutex 가 담당하며 서버는 락을 잡지 않는다
// (락을 잡은 채 네트워크 I/O 를 하지 않는다 - CLAUDE.md 절대 규칙 3).
//
// 엔드포인트: POST /query (본문: SQL 텍스트 또는 {"sql":"..."}), GET /health.
class HttpServer {
 public:
  HttpServer(Engine* engine, ServerOptions options);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  Status start();
  uint16_t port() const { return port_; }
  // 새 연결 수락을 멈추고, 큐에 남은 요청까지 처리한 뒤 종료한다 (graceful). 여러 번 호출해도 안전.
  void stop();

 private:
  void accept_loop();
  void handle_connection(int fd);  // fd 소유: serve() 를 호출하고 항상 close
  void serve(int fd);
  void reject_overloaded(int fd);
  void respond(int fd, int status, const std::string& body);

  Engine* engine_;
  ServerOptions options_;
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> stopped_{false};
  std::thread acceptor_;
  std::unique_ptr<ThreadPool> pool_;
};

}  // namespace minidb
