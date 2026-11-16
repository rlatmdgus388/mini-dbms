#include "server/http_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "server/api.h"
#include "server/http.h"

namespace minidb {

namespace {

void send_all(int fd, const std::string& data) {
#ifdef MSG_NOSIGNAL
  const int flags = MSG_NOSIGNAL;
#else
  const int flags = 0;
#endif
  size_t done = 0;
  while (done < data.size()) {
    ssize_t n = ::send(fd, data.data() + done, data.size() - done, flags);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;  // 클라이언트가 끊었거나 타임아웃: 응답을 포기한다
    }
    done += static_cast<size_t>(n);
  }
}

void set_timeouts(int fd, int sec) {
  timeval tv{};
  tv.tv_sec = sec;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

// 응답 후 연결 종료를 준비한다(fd 는 handle_connection 이 닫는다). 읽지 않은 요청 데이터가 남아 있는 채로 close 하면 RST 가 나가 클라이언트가
// 응답을 잃을 수 있으므로, 쓰기 방향만 먼저 닫고 남은 입력을 조금 비운다.
void close_gracefully(int fd) {
  ::shutdown(fd, SHUT_WR);
  timeval tv{};
  tv.tv_usec = 200 * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  char buf[4096];
  for (int i = 0; i < 256; ++i) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
  }
}

}  // namespace

HttpServer::HttpServer(Engine* engine, ServerOptions options) : engine_(engine), options_(std::move(options)) {}

HttpServer::~HttpServer() { stop(); }

Status HttpServer::start() {
  ::signal(SIGPIPE, SIG_IGN);  // 끊어진 소켓에 쓸 때 프로세스가 죽지 않도록
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return Status::fail(ErrorCode::kIo, std::string("socket: ") + std::strerror(errno));
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(options_.port);
  if (::inet_pton(AF_INET, options_.host.c_str(), &addr.sin_addr) != 1) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return Status::fail(ErrorCode::kInvalid, "invalid host address '" + options_.host + "'");
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 128) != 0) {
    Status s = Status::fail(ErrorCode::kIo, "cannot listen on " + options_.host + ":" + std::to_string(options_.port) + ": " + std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return s;
  }
  socklen_t len = sizeof(addr);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);

  pool_ = std::make_unique<ThreadPool>(options_.threads, options_.max_queue);
  acceptor_ = std::thread([this] { accept_loop(); });
  return Status::success();
}

void HttpServer::stop() {
  if (stopped_.exchange(true)) return;
  stop_requested_ = true;
  if (acceptor_.joinable()) acceptor_.join();
  if (pool_) pool_->shutdown();  // 큐에 남은 요청까지 처리한 뒤 워커 종료
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void HttpServer::accept_loop() {
  while (!stop_requested_) {
    pollfd p{listen_fd_, POLLIN, 0};
    int r = ::poll(&p, 1, 100);  // 종료 플래그를 주기적으로 확인
    if (r <= 0) continue;
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    set_timeouts(fd, options_.io_timeout_sec);
    if (!pool_->submit([this, fd] { handle_connection(fd); })) reject_overloaded(fd);  // 큐 포화: backpressure
  }
}

void HttpServer::reject_overloaded(int fd) {
  send_all(fd, build_http_response(503, error_to_json("server is overloaded, try again later")));
  ::shutdown(fd, SHUT_WR);
  pollfd p{fd, POLLIN, 0};
  if (::poll(&p, 1, 20) > 0) {  // 이미 도착한 요청 바이트를 비워 RST 를 피한다
    char buf[4096];
    ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
  }
  ::close(fd);
}

void HttpServer::respond(int fd, int status, const std::string& body) {
  send_all(fd, build_http_response(status, body));
  close_gracefully(fd);
}

void HttpServer::serve(int fd) {
  // 1) 헤더 읽기
  std::string buf;
  size_t header_end = std::string::npos;
  char chunk[4096];
  while (header_end == std::string::npos) {
    if (buf.size() > kMaxHeaderBytes) return respond(fd, 431, error_to_json("request headers too large"));
    ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n == 0) {  // 요청을 다 보내기 전에 클라이언트가 끊음
      return;
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return respond(fd, 408, error_to_json("request timeout"));
      return;
    }
    buf.append(chunk, static_cast<size_t>(n));
    header_end = buf.find("\r\n\r\n");
  }
  if (header_end > kMaxHeaderBytes) return respond(fd, 431, error_to_json("request headers too large"));

  HttpRequest req;
  size_t content_length = 0;
  std::string error;
  int bad = parse_request_head(buf.substr(0, header_end), &req, &content_length, &error);
  if (bad != 0) return respond(fd, bad, error_to_json(error));

  // 2) 라우팅 (본문 크기 제한은 본문을 읽기 전에 검사)
  bool is_query = req.path == "/query";
  bool is_health = req.path == "/health";
  if (!is_query && !is_health) return respond(fd, 404, error_to_json("not found"));
  if (is_health && req.method != "GET") return respond(fd, 405, error_to_json("use GET /health"));
  if (is_query && req.method != "POST") return respond(fd, 405, error_to_json("use POST /query"));
  if (content_length > options_.max_body_bytes) return respond(fd, 413, error_to_json("request body too large"));

  if (is_health) {
    return respond(fd, 200, "{\"ok\":true,\"status\":\"up\",\"threads\":" + std::to_string(pool_->num_threads()) +
                                ",\"queued\":" + std::to_string(pool_->pending()) + "}");
  }

  // 3) 본문 읽기
  req.body = buf.substr(header_end + 4);
  if (req.body.size() > content_length) req.body.resize(content_length);
  while (req.body.size() < content_length) {
    ssize_t n = ::recv(fd, chunk, std::min(sizeof(chunk), content_length - req.body.size()), 0);
    if (n == 0) return;
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return respond(fd, 408, error_to_json("request timeout"));
      return;
    }
    req.body.append(chunk, static_cast<size_t>(n));
  }

  // 4) SQL 실행: 락은 Engine/Table 안에서만 잡히고, 소켓 I/O 는 실행이 끝난 뒤에 한다.
  auto ct = req.headers.find("content-type");
  Result<std::string> sql = extract_sql(ct == req.headers.end() ? "" : ct->second, req.body);
  if (!sql.ok()) return respond(fd, http_status_for(sql.status().code()), error_to_json(sql.status().message()));

  auto start = std::chrono::steady_clock::now();
  ExecOutcome outcome = engine_->execute(sql.value());
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  int status = outcome.status.ok() ? 200 : http_status_for(outcome.status.code());
  respond(fd, status, outcome_to_json(outcome, ms));
}

}  // namespace minidb

namespace minidb {

// fd 의 소유권은 여기에 있다: 어떤 경로(예외 포함)로 끝나도 정확히 한 번 닫는다.
void HttpServer::handle_connection(int fd) {
  try {
    serve(fd);
  } catch (...) {
    try {
      send_all(fd, build_http_response(500, error_to_json("internal server error")));
    } catch (...) {
    }
  }
  ::close(fd);
}

}  // namespace minidb
