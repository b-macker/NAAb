#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace naab {
namespace runtime {

// Thread pool for polyglot async execution
// Limits concurrent threads to avoid exhaustion on Android
class ThreadPool {
public:
    // Create thread pool with specified number of worker threads.
    // Default: 8 workers. max_queue_size (default 1000) caps the pending task
    // queue — enqueue() throws when the queue is full (V-ASYNC-002).
    explicit ThreadPool(size_t num_threads = 8, size_t max_queue_size = 1000);

    ~ThreadPool();

    // Submit a task and get a future for the result.
    // Throws std::runtime_error if the queue is full (>= max_queue_size tasks pending).
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type>;

    // Get number of worker threads
    size_t getNumThreads() const { return workers_.size(); }

    // Get number of queued tasks
    size_t getQueuedTasks() const;

    // Check if pool is shutting down
    bool isShuttingDown() const { return stop_; }

private:
    // Worker threads
    std::vector<std::thread> workers_;

    // Task queue
    std::queue<std::function<void()>> tasks_;

    // Synchronization
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;

    // Shutdown flag
    bool stop_;

    // V-ASYNC-002: maximum number of tasks allowed in the queue at once.
    // Prevents memory exhaustion when async tasks are spawned in a tight loop.
    size_t max_queue_size_;
};

// Template implementation must be in header
template<typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<typename std::result_of<F(Args...)>::type>
{
    using return_type = typename std::result_of<F(Args...)>::type;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> res = task->get_future();

    {
        std::unique_lock<std::mutex> lock(queue_mutex_);

        // Don't allow enqueueing after stopping the pool
        if (stop_) {
            throw std::runtime_error("ThreadPool: Cannot enqueue on stopped pool");
        }

        // V-ASYNC-002: reject when queue is full to prevent memory exhaustion.
        // Caller (async dispatch) should propagate this as a runtime error.
        if (tasks_.size() >= max_queue_size_) {
            throw std::runtime_error(
                "Async queue full: " + std::to_string(max_queue_size_) +
                " tasks pending. Await existing tasks before enqueuing more.\n\n"
                "  Fix: add 'await' calls between async spawning loops,\n"
                "  or reduce the number of concurrent async tasks.\n"
            );
        }

        tasks_.emplace([task]() { (*task)(); });
    }

    condition_.notify_one();
    return res;
}

} // namespace runtime
} // namespace naab
