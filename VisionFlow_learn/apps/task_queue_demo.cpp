#include "image_task_queue.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace jobs = visionflow::jobs;
namespace protocol = visionflow::protocol;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 仅用于本课观察：启动一个消费者，关闭入口，排空队列并回收线程。
std::vector<std::uint64_t> drain_with_worker(jobs::ImageTaskQueue& queue) {
    std::vector<std::uint64_t> processed;
    std::exception_ptr worker_error;
    std::thread worker([&] {
        try {
            for (;;) {
                jobs::ImageTask task;
                if (!queue.wait_pop(task)) {
                    break;
                }
                // 在队列锁之外使用取出的任务。本课只记录编号，不解码图片。
                processed.push_back(task.header.request_id);
            }
        } catch (...) {
            worker_error = std::current_exception();
        }
    });

    try {
        queue.close();
        worker.join();
    } catch (...) {
        queue.close();
        if (worker.joinable()) {
            worker.join();
        }
        throw;
    }
    if (worker_error) {
        std::rethrow_exception(worker_error);
    }
    return processed;
}

} // namespace

int main() {
    try {
        jobs::ImageTaskQueue queue(2);
        jobs::ImageTask first{
            11, {protocol::Type::image, 101, 4}, {10, 20, 30, 40}
        };
        jobs::ImageTask second{
            12, {protocol::Type::image, 102, 3}, {50, 60, 70}
        };
        jobs::ImageTask third{
            13, {protocol::Type::image, 103, 2}, {80, 90}
        };

        require(queue.try_push(std::move(first)) == jobs::SubmitResult::accepted,
                "第一个任务应被接受");
        require(queue.try_push(std::move(second)) == jobs::SubmitResult::accepted,
                "第二个任务应被接受");
        std::cout << "容量为 2，已接受任务 101、102\n";

        require(queue.try_push(std::move(third)) == jobs::SubmitResult::full,
                "队列已满时应拒绝第三个任务");
        require(third.image_bytes == std::vector<std::uint8_t>({80, 90}),
                "拒绝提交后，调用者的图片字节应保持原样");
        std::cout << "任务 103 被拒绝：队列已满，原来的 2 字节仍在\n";

        jobs::ImageTask taken;
        require(queue.wait_pop(taken), "队列中应能取出第一个任务");
        require(taken.connection_token == 11 &&
                    taken.header.request_id == 101 &&
                    taken.image_bytes ==
                        std::vector<std::uint8_t>({10, 20, 30, 40}),
                "取出的任务内容不正确");
        std::cout << "取出任务 101，释放一个排队位置\n";

        require(queue.try_push(std::move(third)) == jobs::SubmitResult::accepted,
                "释放位置后，第三个任务应被接受");
        std::cout << "重新提交任务 103：已接受\n";

        const auto processed = drain_with_worker(queue);
        require(processed == std::vector<std::uint64_t>({102, 103}),
                "关闭后应按入队顺序排空剩余任务");
        std::cout << "关闭入口后，工作线程依次取出任务 102、103 并退出\n";

        jobs::ImageTask late{
            14, {protocol::Type::image, 104, 1}, {100}
        };
        require(queue.try_push(std::move(late)) == jobs::SubmitResult::closed,
                "关闭后必须拒绝新任务");
        require(late.image_bytes == std::vector<std::uint8_t>({100}),
                "关闭后拒绝提交不应取走任务内容");
        require(!queue.wait_pop(taken), "关闭且排空后应返回 false");
        queue.close();
        std::cout << "关闭后拒绝新任务；队列排空后取任务返回 false\n";

        jobs::ImageTaskQueue empty_queue(1);
        require(drain_with_worker(empty_queue).empty(),
                "空队列关闭不应产生任务");
        std::cout << "空队列关闭后，工作线程也能退出\n";
        std::cout << "有界任务队列观察完成\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "队列观察失败：" << error.what() << '\n';
        return 1;
    }
}
