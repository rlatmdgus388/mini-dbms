#include "server/thread_pool.h"

namespace minidb {

ThreadPool::ThreadPool(size_t num_threads, size_t max_queue) : max_queue_(max_queue) {
  if (num_threads == 0) num_threads = 1;
  workers_.reserve(num_threads);
  for (size_t i = 0; i < num_threads; ++i) workers_.emplace_back([this] { worker_loop(); });
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::submit(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || queue_.size() >= max_queue_) return false;
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
  return true;
}

void ThreadPool::shutdown() {
  std::lock_guard<std::mutex> join_lock(join_mutex_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cv_.notify_all();
  for (std::thread& t : workers_)
    if (t.joinable()) t.join();
}

size_t ThreadPool::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void ThreadPool::worker_loop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) return;  // stopping_ 이고 더 처리할 작업 없음
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      task();
    } catch (...) {
      // 작업의 예외가 워커 스레드를 죽이지 않도록 삼킨다.
    }
  }
}

}  // namespace minidb
