#pragma once

#include "image_task_queue.hpp"
#include "tcp_io.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace visionflow::jobs {

struct CompletedTask {
    std::uint64_t connection_token = 0;
    std::size_t worker_index = 0;
    std::vector<std::uint8_t> output;
    std::exception_ptr error;
};

class ImageWorkerPool {
public:
    using Handler = std::function<std::vector<std::uint8_t>(
        const protocol::Header&, const std::vector<std::uint8_t>&)>;

    static constexpr std::size_t worker_count = 2;
    static constexpr std::size_t queue_capacity = 2;
    static constexpr std::size_t max_outstanding =
        worker_count + queue_capacity;

    explicit ImageWorkerPool(
        Handler handler,
        std::chrono::milliseconds demo_delay = std::chrono::milliseconds{0});
    ~ImageWorkerPool();

    ImageWorkerPool(const ImageWorkerPool&) = delete;
    ImageWorkerPool& operator=(const ImageWorkerPool&) = delete;

    // 以下接口只由事件循环线程调用。
    SubmitResult try_submit(ImageTask&& task);
    int notification_fd() const { return notification_.get(); }
    void drain_notifications();
    std::deque<CompletedTask> take_completed();
    void check_health() const;
    void stop_accepting();
    std::size_t outstanding() const noexcept { return outstanding_; }


private:
    void worker_main(std::size_t index) noexcept;
    void notify_loop() noexcept;
    void join_workers();

    ImageTaskQueue tasks_{queue_capacity};
    Handler handler_;
    const std::chrono::milliseconds demo_delay_;
    net::SocketFd notification_;
    std::mutex completed_mutex_;
    std::deque<CompletedTask> completed_;
    std::atomic<bool> fatal_{false};
    std::vector<std::thread> workers_;
    std::size_t outstanding_ = 0; // 只由事件循环线程读写
};

} // namespace visionflow::jobs
