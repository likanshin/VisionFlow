#include "visionflow/core/logger.hpp"
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
namespace visionflow {
Logger& Logger::instance() { static Logger logger; return logger; }
void Logger::set_level(const std::string& level) { std::lock_guard<std::mutex> lock(mutex_); errors_only_ = level == "error"; }
void Logger::log(const std::string& level, const std::string& message, std::uint64_t session, std::uint64_t request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (errors_only_ && level != "error") return;
    std::ostringstream thread; thread << std::this_thread::get_id();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::cerr << nlohmann::json{{"time_ms", ms}, {"level", level}, {"thread", thread.str()},
        {"session_id", session}, {"request_id", request}, {"message", message}}.dump() << '\n';
}
}
