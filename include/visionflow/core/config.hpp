#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
namespace visionflow {
struct Config {
    std::string listen_address = "127.0.0.1", mode = "realtime", processor = "lane_lines", log_level = "info";
    int port = 9000, worker_threads = 4, max_clients = 8, max_inflight_per_client = 8;
    int idle_timeout_ms = 30000, io_timeout_ms = 10000, shutdown_timeout_ms = 5000;
    int max_width = 4096, max_height = 4096, max_lines = 128;
    std::uint32_t max_payload_bytes = 16 * 1024 * 1024;
    std::size_t max_pixels = 2073600, task_queue_capacity = 32, task_queue_bytes = 64 * 1024 * 1024;
    std::size_t result_queue_capacity = 64, result_queue_bytes = 4 * 1024 * 1024;
    std::size_t max_send_buffer_bytes = 1024 * 1024, memory_budget_bytes = 640 * 1024 * 1024;
    void validate() const;
    static Config load(const std::string& path);
};
}
