#pragma once
#include "visionflow/concurrency/bounded_queue.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace visionflow {
template<class T> class ThreadPool {
public:
    using Handler = std::function<void(T&&, std::size_t)>;
    ThreadPool(BoundedQueue<T>& queue, std::size_t count, Handler handler)
        : queue_(queue) {
        if (!count) throw std::invalid_argument("zero worker count");
        try {
            for (std::size_t i = 0; i < count; ++i) {
                threads_.emplace_back([this, handler, i] {
                    while (auto item = queue_.pop()) {
                        try { handler(std::move(*item), i); }
                        catch (...) { ++failures_; } // Contain all task exceptions.
                    }
                });
            }
        } catch (...) { queue_.close(true); join(); throw; }
    }
    ~ThreadPool() { queue_.close(); join(); }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    void join() { for (auto& t : threads_) if (t.joinable()) t.join(); }
    std::uint64_t failures() const { return failures_.load(); }
private:
    BoundedQueue<T>& queue_;
    std::vector<std::thread> threads_;
    std::atomic<std::uint64_t> failures_{0};
};
}
