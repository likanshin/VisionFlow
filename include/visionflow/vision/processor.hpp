#pragma once
#include "visionflow/core/config.hpp"
#include <memory>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <string>
#include <vector>
namespace visionflow {
struct Frame { cv::Mat image; std::uint64_t request_id = 0; };
struct ProcessResult {
    std::string processor;
    int width = 0, height = 0, edge_pixels = 0;
    std::vector<cv::Vec4i> lines;
    cv::Mat preview;
    nlohmann::json json(std::uint64_t request_id) const;
};
class IProcessor {
public:
    virtual ~IProcessor() = default;
    virtual ProcessResult process(const Frame& frame) = 0;
};
std::unique_ptr<IProcessor> create_processor(const std::string& name, int max_lines = 128);
// Inspect JPEG SOF dimensions before invoking the image allocator.
cv::Mat decode_jpeg(const std::vector<std::uint8_t>& bytes, const Config& config);
}
