// Thread Pool Implementation for Polyglot Async Execution

#include "naab/thread_pool.h"
#include <fmt/core.h>

namespace naab {
namespace runtime {

ThreadPool::ThreadPool(size_t num_threads)
    : stop_(false)
{
    // Create worker threads
    workers_.reserve(num_threads);

    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this, i] {
            // NOTE: Workers do NOT initialize Python thread states.
            // Python executes sequentially on the main thread (lang_supported=false)
            // to avoid CFI shadow entry fragmentation that breaks posix_spawn on Android.
            // Workers only handle JS, Shell, Rust, C++, C# tasks.

            // Worker thread loop
            while (true) {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex_);

                    // Wait for task or shutdown signal
                    this->condition_.wait(lock, [this] {
                        return this->stop_ || !this->tasks_.empty();
                    });

                    // Exit if shutting down and no tasks remain
                    if (this->stop_ && this->tasks_.empty()) {
                        return;
                    }

                    // Get next task
                    if (!this->tasks_.empty()) {
                        task = std::move(this->tasks_.front());
                        this->tasks_.pop();
                    }
                }

                // Execute task outside the lock
                if (task) {
                    try {
                        task();
                    } catch (const std::exception& e) {
                        fmt::print("[ThreadPool] Worker {} caught exception: {}\n", i, e.what());
                    } catch (...) {
                        fmt::print("[ThreadPool] Worker {} caught unknown exception\n", i);
                    }
                }
            }
        });
    }

}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }

    // Wake up all workers
    condition_.notify_all();

    // Wait for all workers to finish (they clean up Python states before exiting)
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

}

size_t ThreadPool::getQueuedTasks() const {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

} // namespace runtime
} // namespace naab
