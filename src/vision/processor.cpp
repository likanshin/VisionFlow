#include "visionflow/vision/processor.hpp"
#include <algorithm>
#include <cmath>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
namespace visionflow {
nlohmann::json ProcessResult::json(std::uint64_t id) const {
    auto result = nlohmann::json{{"request_id", id}, {"status", "ok"}, {"processor", processor},
        {"width", width}, {"height", height}, {"edge_pixels", edge_pixels}, {"lines", nlohmann::json::array()}};
    for (const auto& line : lines) result["lines"].push_back({line[0], line[1], line[2], line[3]});
    return result;
}
namespace {
class Processor final : public IProcessor {
public:
    Processor(std::string name, int max_lines) : name_(std::move(name)), max_lines_(max_lines) {}
    ProcessResult process(const Frame& frame) override {
        if (frame.image.empty() || frame.image.type() != CV_8UC3) throw std::invalid_argument("expected BGR image");
        ProcessResult result;
        result.processor = name_; result.width = frame.image.cols; result.height = frame.image.rows;
        cv::Mat gray, edges;
        cv::cvtColor(frame.image, gray, cv::COLOR_BGR2GRAY);
        cv::GaussianBlur(gray, gray, {5, 5}, 1.5);
        cv::Canny(gray, edges, 50, 150);
        result.edge_pixels = cv::countNonZero(edges);
        if (name_ == "edges") {
            cv::cvtColor(edges, result.preview, cv::COLOR_GRAY2BGR);
        } else {
            cv::Mat mask = cv::Mat::zeros(edges.size(), CV_8UC1);
            const int w = result.width, h = result.height;
            std::vector<cv::Point> polygon{{w / 10, h - 1}, {w * 45 / 100, h * 55 / 100},
                {w * 55 / 100, h * 55 / 100}, {w * 9 / 10, h - 1}};
            cv::fillConvexPoly(mask, polygon, cv::Scalar(255));
            cv::bitwise_and(edges, mask, edges);
            std::vector<cv::Vec4i> candidates;
            cv::HoughLinesP(edges, candidates, 1, CV_PI / 180, 25, std::max(15, h / 12), 20);
            std::sort(candidates.begin(), candidates.end(), [](const cv::Vec4i& a, const cv::Vec4i& b) {
                const auto length = [](const cv::Vec4i& v) { return std::hypot(v[2] - v[0], v[3] - v[1]); };
                if (length(a) != length(b)) return length(a) > length(b);
                return std::lexicographical_compare(a.val, a.val + 4, b.val, b.val + 4);
            });
            result.preview = frame.image.clone();
            for (const auto& line : candidates) {
                const double dx = line[2] - line[0], dy = line[3] - line[1];
                if (std::abs(dy) < std::abs(dx) * 0.35) continue;
                result.lines.push_back(line);
                cv::line(result.preview, {line[0], line[1]}, {line[2], line[3]}, {0, 255, 0}, 2);
                if (result.lines.size() >= static_cast<std::size_t>(max_lines_)) break;
            }
        }
        return result;
    }
private:
    std::string name_;
    int max_lines_;
};
}
std::unique_ptr<IProcessor> create_processor(const std::string& name, int max_lines) {
    if (name != "edges" && name != "lane_lines") throw std::invalid_argument("unknown processor: " + name);
    if (max_lines < 1 || max_lines > 256) throw std::invalid_argument("invalid max_lines");
    return std::make_unique<Processor>(name, max_lines);
}
cv::Mat decode_jpeg(const std::vector<std::uint8_t>& b, const Config& c) {
    if (b.size() < 4 || b.size() > c.max_payload_bytes || b[0] != 0xff || b[1] != 0xd8)
        throw std::invalid_argument("invalid JPEG payload");
    int width = 0, height = 0;
    std::size_t p = 2;
    while (p < b.size()) {
        if (b[p++] != 0xff) throw std::invalid_argument("invalid JPEG marker");
        while (p < b.size() && b[p] == 0xff) ++p;
        if (p == b.size()) break;
        const auto marker = b[p++];
        if (marker == 0xda || marker == 0xd9) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (p + 2 > b.size()) break;
        const std::size_t length = (static_cast<std::size_t>(b[p]) << 8U) | b[p + 1];
        if (length < 2 || length > b.size() - p) throw std::invalid_argument("truncated JPEG segment");
        const bool sof = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
        if (sof) {
            // Only baseline and progressive 8-bit Huffman JPEG are supported.
            if ((marker != 0xc0 && marker != 0xc2) || length < 8 || b[p + 2] != 8 || width != 0)
                throw std::invalid_argument("unsupported JPEG frame");
            height = (b[p + 3] << 8) | b[p + 4]; width = (b[p + 5] << 8) | b[p + 6];
            if (width <= 0 || height <= 0 || width > c.max_width || height > c.max_height
                || static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > c.max_pixels)
                throw std::invalid_argument("decoded image dimensions exceed limits");
        }
        p += length;
    }
    if (!width || !height) throw std::invalid_argument("missing JPEG dimensions");
    auto image = cv::imdecode(b, cv::IMREAD_COLOR | cv::IMREAD_IGNORE_ORIENTATION);
    if (image.empty() || image.cols != width || image.rows != height) throw std::invalid_argument("JPEG decoding failed");
    return image;
}
}
