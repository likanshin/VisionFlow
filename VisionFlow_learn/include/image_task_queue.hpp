#pragma once

#include "protocol.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace visionflow::jobs {

struct ImageTask {
    std::uint64_t connection_token = 0;
    protocol::Header header{};
    std::vector<std::uint8_t> image_bytes;
};

enum class SubmitResult {
    accepted,
    full,
    closed
};

class ImageTaskQueue {
public:
    explicit ImageTaskQueue(std::size_t capacity);

    ImageTaskQueue(const ImageTaskQueue&) = delete;
    ImageTaskQueue& operator=(const ImageTaskQueue&) = delete;

    SubmitResult try_push(ImageTask&& task);
    bool wait_pop(ImageTask& task);
    void close();

private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<ImageTask> tasks_;
    bool closed_ = false;
};

} // namespace visionflow::jobs
