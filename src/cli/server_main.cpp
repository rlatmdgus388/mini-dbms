// minidb_server: Phase 1+2 엔진을 Thread Pool 기반 HTTP API 로 노출한다.
#include <signal.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "server/http_server.h"

using namespace minidb;

namespace {
volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void usage() {
  std::fprintf(stderr,
               "usage: minidb_server [--data-dir DIR] [--host ADDR] [--port N] [--threads N] [--max-queue N]\n"
               "  defaults: --data-dir data --host 127.0.0.1 --port 8080 --threads 8 --max-queue 256\n");
}

bool parse_number(const char* s, long min, long max, long* out) {
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < min || v > max) return false;
  *out = v;
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  std::string data_dir = "data";
  ServerOptions opts;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a value\n", a.c_str());
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    long n = 0;
    if (a == "--data-dir") data_dir = value();
    else if (a == "--host") opts.host = value();
    else if (a == "--port") {
      if (!parse_number(value(), 0, 65535, &n)) { std::fprintf(stderr, "error: invalid --port\n"); return 2; }
      opts.port = static_cast<uint16_t>(n);
    } else if (a == "--threads") {
      if (!parse_number(value(), 1, 1024, &n)) { std::fprintf(stderr, "error: invalid --threads\n"); return 2; }
      opts.threads = static_cast<size_t>(n);
    } else if (a == "--max-queue") {
      if (!parse_number(value(), 1, 1000000, &n)) { std::fprintf(stderr, "error: invalid --max-queue\n"); return 2; }
      opts.max_queue = static_cast<size_t>(n);
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
      usage();
      return 2;
    }
  }

  Result<std::unique_ptr<Engine>> engine = Engine::open(data_dir);
  if (!engine.ok()) {
    std::fprintf(stderr, "ERROR: %s\n", engine.status().message().c_str());
    return 1;
  }
  Status st = engine.value()->load_all_tables();  // 시작 시 모든 테이블을 열어 인덱스를 미리 구축
  if (!st.ok()) {
    std::fprintf(stderr, "ERROR: %s\n", st.message().c_str());
    return 1;
  }

  HttpServer server(engine.value().get(), opts);
  st = server.start();
  if (!st.ok()) {
    std::fprintf(stderr, "ERROR: %s\n", st.message().c_str());
    return 1;
  }
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  std::printf("listening on %s:%u (threads=%zu, max_queue=%zu)\n", opts.host.c_str(), server.port(), opts.threads, opts.max_queue);
  std::fflush(stdout);
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::printf("shutting down...\n");
  server.stop();
  return 0;
}
