#pragma once
#include <cstdint>
#include <mutex>
#include <string>
namespace visionflow {
class Logger {
public:
    static Logger& instance();
    void set_level(const std::string& level);
    void log(const std::string& level, const std::string& message, std::uint64_t session = 0, std::uint64_t request = 0);
private:
    std::mutex mutex_;
    bool errors_only_ = false;
};
}
