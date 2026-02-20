#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>

namespace naab {
namespace runtime {

// Thread pool for polyglot async execution
// Limits concurrent threads to avoid exhaustion on Android
class ThreadPool {
public:
    // Create thread pool with specified number of worker threads
    // Default: 8 workers (good balance for most devices)
    explicit ThreadPool(size_t num_threads = 8);

    ~ThreadPool();

    // Submit a task and get a future for the result
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

        tasks_.emplace([task]() { (*task)(); });
    }

    condition_.notify_one();
    return res;
}

} // namespace runtime
} // namespace naab
