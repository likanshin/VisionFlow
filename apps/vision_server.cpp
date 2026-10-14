#include "visionflow/net/server.hpp"
#include "visionflow/core/logger.hpp"
#include <iostream>
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "vision_server --config configs/server.json\n"; return 0;
        }
        if (argc != 3 || std::string(argv[1]) != "--config") throw std::invalid_argument("usage: vision_server --config FILE");
        const auto config = visionflow::Config::load(argv[2]);
        visionflow::Logger::instance().set_level(config.log_level);
        return visionflow::run_server(config);
    } catch (const std::exception& e) {
        visionflow::Logger::instance().log("error", e.what()); return 1;
    }
}
