#include "image_worker_pool.hpp"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <sys/eventfd.h>
#include <unistd.h>

namespace visionflow::jobs {

void ImageWorkerPool::stop_accepting() {
    tasks_.close();
}


ImageWorkerPool::ImageWorkerPool(
    Handler handler, std::chrono::milliseconds demo_delay)
    : handler_(std::move(handler)),
      demo_delay_(demo_delay),
      notification_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (!handler_ || demo_delay_.count() < 0 || demo_delay_.count() > 5000) {
        throw std::invalid_argument("处理函数不能为空，观察延迟必须为 0～5000 毫秒");
    }
    workers_.reserve(worker_count);
    try {
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this, i] { worker_main(i + 1); });
        }
    } catch (...) {
        tasks_.close();
        join_workers();
        throw;
    }
}

ImageWorkerPool::~ImageWorkerPool() {
    tasks_.close();
    join_workers();
}

void ImageWorkerPool::join_workers() {
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

SubmitResult ImageWorkerPool::try_submit(ImageTask&& task) {
    check_health();
    if (outstanding_ >= max_outstanding) {
        return SubmitResult::full;
    }
    const auto result = tasks_.try_push(std::move(task));
    if (result == SubmitResult::accepted) {
        ++outstanding_;
    }
    return result;
}

void ImageWorkerPool::check_health() const {
    if (fatal_.load()) {
        throw std::runtime_error("工作线程或完成通知发生不可恢复错误");
    }
}

void ImageWorkerPool::notify_loop() noexcept {
    const std::uint64_t one = 1;
    for (;;) {
        const auto count = ::write(notification_.get(), &one, sizeof(one));
        if (count == static_cast<ssize_t>(sizeof(one))) {
            return;
        }
        const int error = errno;
        if (count < 0 && error == EINTR) {
            continue;
        }
        if (count < 0 && (error == EAGAIN || error == EWOULDBLOCK)) {
            return; // 计数器已满，本来就处于可读状态。
        }
        fatal_.store(true);
        return;
    }
}

void ImageWorkerPool::worker_main(std::size_t index) noexcept {
    try {
        for (;;) {
            CompletedTask done;
            {
                ImageTask task;
                if (!tasks_.wait_pop(task)) {
                    return;
                }
                done.connection_token = task.connection_token;
                done.worker_index = index;
                try {
                    if (demo_delay_.count() != 0) {
                        std::this_thread::sleep_for(demo_delay_);
                    }
                    done.output = handler_(task.header, task.image_bytes);
                } catch (...) {
                    done.error = std::current_exception();
                }
            } // 本任务的压缩图片在这里释放。
            {
                std::lock_guard<std::mutex> lock(completed_mutex_);
                completed_.push_back(std::move(done));
            }
            notify_loop();
        }
    } catch (...) {
        // 如完成队列分配失败，无法再保证交回一条完整结果。
        fatal_.store(true);
        notify_loop();
    }
}

void ImageWorkerPool::drain_notifications() {
    std::uint64_t count = 0;
    for (;;) {
        const auto received = ::read(notification_.get(), &count, sizeof(count));
        if (received == static_cast<ssize_t>(sizeof(count))) {
            continue; // 数值不用于决定取多少条结果。
        }
        const int error = errno;
        if (received < 0 && error == EINTR) {
            continue;
        }
        if (received < 0 && (error == EAGAIN || error == EWOULDBLOCK)) {
            return;
        }
        if (received < 0) {
            throw std::system_error(error, std::generic_category(),
                                    "读取完成通知失败");
        }
        throw std::runtime_error("完成通知长度不正确");
    }
}

std::deque<CompletedTask> ImageWorkerPool::take_completed() {
    check_health();
    std::deque<CompletedTask> ready;
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        ready.swap(completed_);
    }
    if (ready.size() > outstanding_) {
        throw std::logic_error("未交回任务计数不一致");
    }
    outstanding_ -= ready.size();
    return ready;
}

} // namespace visionflow::jobs
