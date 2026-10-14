#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace visionflow {
// Weight includes payload bytes; all entries must also fit the item limit.
template<class T> class BoundedQueue {
    static_assert(std::is_nothrow_move_constructible<T>::value, "T must move without throwing");
    struct Entry { T value; std::size_t bytes; };
public:
    BoundedQueue(std::size_t capacity, std::size_t byte_capacity)
        : capacity_(capacity), byte_capacity_(byte_capacity) {
        if (!capacity || !byte_capacity) throw std::invalid_argument("zero queue capacity");
    }
    bool try_push(T&& value, std::size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || !fits(bytes)) return false;
        insert(std::move(value), bytes);
        return true;
    }
    bool push(T&& value, std::size_t bytes) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (bytes > byte_capacity_) return false;
        space_.wait(lock, [&] { return closed_ || fits(bytes); });
        if (closed_) return false;
        insert(std::move(value), bytes);
        return true;
    }
    // The caller receives evicted jobs and must notify their clients.
    bool push_latest(T&& value, std::size_t bytes, std::vector<T>& evicted) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || bytes > byte_capacity_) return false;
        while (!fits(bytes)) {
            evicted.push_back(std::move(queue_.front().value));
            bytes_ -= queue_.front().bytes;
            queue_.pop_front();
        }
        insert(std::move(value), bytes);
        return true;
    }
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        return take();
    }
    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        return take();
    }
    void close(bool cancel = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        if (cancel) { queue_.clear(); bytes_ = 0; }
        ready_.notify_all();
        space_.notify_all();
    }
    std::size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return queue_.size(); }
    std::size_t peak() const { std::lock_guard<std::mutex> lock(mutex_); return peak_; }
private:
    bool fits(std::size_t n) const { return queue_.size() < capacity_ && n <= byte_capacity_ - bytes_; }
    void insert(T&& value, std::size_t n) {
        queue_.push_back({std::move(value), n});
        bytes_ += n;
        peak_ = std::max(peak_, queue_.size());
        ready_.notify_one();
    }
    std::optional<T> take() {
        if (queue_.empty()) return std::nullopt;
        T value = std::move(queue_.front().value);
        bytes_ -= queue_.front().bytes;
        queue_.pop_front();
        space_.notify_all();
        return value;
    }
    const std::size_t capacity_, byte_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_, space_;
    std::deque<Entry> queue_;
    std::size_t bytes_ = 0, peak_ = 0;
    bool closed_ = false;
};
}
