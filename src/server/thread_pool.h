#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace minidb {

// 고정 크기 스레드 풀 + 유한 길이 작업 큐.
//  - submit(): 큐가 가득 찼거나 종료 중이면 false (호출자가 503 등으로 backpressure 처리).
//  - shutdown(): 새 작업을 거부하고, 큐에 남은 작업을 모두 처리한 뒤 워커를 join 한다 (graceful).
//  - 작업에서 예외가 새어 나와도 워커는 죽지 않는다 (CLAUDE.md 절대 규칙 7).
class ThreadPool {
 public:
  ThreadPool(size_t num_threads, size_t max_queue);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  bool submit(std::function<void()> task);
  void shutdown();
  size_t pending() const;
  size_t num_threads() const { return workers_.size(); }

 private:
  void worker_loop();

  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::mutex join_mutex_;  // shutdown() 동시 호출 보호
  const size_t max_queue_;
  bool stopping_ = false;
};

}  // namespace minidb
