#include "image_task_queue.hpp"

#include <stdexcept>
#include <utility>

namespace visionflow::jobs {

ImageTaskQueue::ImageTaskQueue(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("任务队列容量必须大于 0");
    }
}

SubmitResult ImageTaskQueue::try_push(ImageTask&& task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return SubmitResult::closed;
        }
        if (tasks_.size() >= capacity_) {
            return SubmitResult::full;
        }
        tasks_.push_back(std::move(task));
    }
    not_empty_.notify_one();
    return SubmitResult::accepted;
}

bool ImageTaskQueue::wait_pop(ImageTask& task) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] {
        return closed_ || !tasks_.empty();
    });
    if (tasks_.empty()) {
        return false;
    }
    task = std::move(tasks_.front());
    tasks_.pop_front();
    return true;
}

void ImageTaskQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    not_empty_.notify_all();
}

} // namespace visionflow::jobs
