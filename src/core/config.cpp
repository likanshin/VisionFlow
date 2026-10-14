#include "visionflow/core/config.hpp"
#include <fstream>
#include <limits>
#include <type_traits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
namespace visionflow {
void Config::validate() const {
    auto require = [](bool ok, const char* why) { if (!ok) throw std::invalid_argument(why); };
    require(port > 0 && port <= 65535, "port must be 1..65535");
    require(worker_threads > 0 && worker_threads <= 64, "worker_threads must be 1..64");
    require(max_clients > 0 && max_clients <= 1024, "max_clients must be 1..1024");
    require(max_inflight_per_client > 0 && max_inflight_per_client <= 256, "max_inflight_per_client must be 1..256");
    require(mode == "realtime" || mode == "offline", "mode must be realtime or offline");
    require(processor == "edges" || processor == "lane_lines", "unknown processor");
    require(log_level == "info" || log_level == "error", "log_level must be info or error");
    require(idle_timeout_ms > 0 && io_timeout_ms > 0 && shutdown_timeout_ms > 0, "timeouts must be positive");
    require(max_width > 0 && max_width <= 16384 && max_height > 0 && max_height <= 16384, "invalid image dimension limit");
    require(max_pixels > 0 && max_pixels <= 64U * 1024U * 1024U, "invalid pixel limit");
    require(max_lines > 0 && max_lines <= 256, "max_lines must be 1..256");
    require(max_payload_bytes > 0 && max_payload_bytes <= 64U * 1024U * 1024U, "payload limit must be 1..64 MiB");
    require(task_queue_capacity > 0 && task_queue_capacity <= 65536 && task_queue_bytes >= max_payload_bytes, "invalid task queue limits");
    require(result_queue_capacity > 0 && result_queue_capacity <= 65536 && result_queue_bytes >= 65536, "invalid result queue limits");
    require(max_send_buffer_bytes >= 65536 && max_send_buffer_bytes <= 64U * 1024U * 1024U, "invalid send buffer limit");
    require(task_queue_bytes <= 4ULL * 1024 * 1024 * 1024 && result_queue_bytes <= 1024ULL * 1024 * 1024, "queue byte budget too large");
    const auto estimate = static_cast<std::uint64_t>(max_clients) * (max_payload_bytes + max_send_buffer_bytes)
        + task_queue_bytes + result_queue_bytes
        + static_cast<std::uint64_t>(worker_threads) * (max_pixels * 32ULL + max_payload_bytes + 65536);
    require(estimate <= memory_budget_bytes, "configured buffer/decode estimate exceeds memory_budget_bytes");
}
Config Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open config: " + path);
    nlohmann::json j;
    file >> j;
    if (!j.is_object()) throw std::invalid_argument("config must be an object");
    Config c;
    std::set<std::string> known;
    auto read = [&](const char* key, auto& value) {
        known.insert(key);
        if (j.contains(key)) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_integral_v<T>) {
                if (!j[key].is_number_integer() || j[key].get<long double>() < 0
                    || j[key].get<long double>() > static_cast<long double>(std::numeric_limits<T>::max()))
                    throw std::invalid_argument(std::string("invalid integer: ") + key);
            }
            value = j[key].get<T>();
        }
    };
#define VF_READ(name) read(#name, c.name)
    VF_READ(listen_address); VF_READ(port); VF_READ(worker_threads); VF_READ(max_clients);
    VF_READ(max_inflight_per_client); VF_READ(mode); VF_READ(processor); VF_READ(log_level);
    VF_READ(max_payload_bytes); VF_READ(max_pixels); VF_READ(max_width); VF_READ(max_height); VF_READ(max_lines);
    VF_READ(task_queue_capacity); VF_READ(task_queue_bytes); VF_READ(result_queue_capacity); VF_READ(result_queue_bytes);
    VF_READ(max_send_buffer_bytes); VF_READ(memory_budget_bytes);
    VF_READ(idle_timeout_ms); VF_READ(io_timeout_ms); VF_READ(shutdown_timeout_ms);
#undef VF_READ
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!known.count(it.key())) throw std::invalid_argument("unknown config key: " + it.key());
    c.validate();
    return c;
}
}
