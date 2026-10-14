#include "visionflow/vision/processor.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <stdexcept>
using namespace visionflow;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool caught = false; try { f(); } catch (const std::exception&) { caught = true; } check(caught, "expected rejection"); }
int main() {
    try {
        cv::setNumThreads(0);
        Config config; config.validate();
        auto invalid = config; invalid.worker_threads = 0; rejects([&] { invalid.validate(); });
        invalid = config; invalid.memory_budget_bytes = 1; rejects([&] { invalid.validate(); });
        cv::Mat image(360, 640, CV_8UC3, cv::Scalar(0));
        cv::line(image, {70, 359}, {285, 200}, {255, 255, 255}, 5);
        cv::line(image, {570, 359}, {355, 200}, {255, 255, 255}, 5);
        std::vector<std::uint8_t> bytes;
        check(cv::imencode(".jpg", image, bytes), "encode fixture");
        const auto decoded = decode_jpeg(bytes, config);
        check(decoded.rows == 360 && decoded.cols == 640, "JPEG size");
        for (const auto& name : {"edges", "lane_lines"}) {
            auto processor = create_processor(name);
            auto result = processor->process({decoded, 42});
            check(result.edge_pixels > 0 && result.json(42)["request_id"] == 42, "processor result");
            if (std::string(name) == "lane_lines") check(!result.lines.empty(), "lane lines detected");
            rejects([&] { processor->process({cv::Mat{}, 1}); });
        }
        rejects([] { create_processor("unknown"); });
        rejects([&] { decode_jpeg({1, 2, 3}, config); });
        auto limited = config; limited.max_pixels = 10;
        rejects([&] { decode_jpeg(bytes, limited); });
        bytes.resize(12); rejects([&] { decode_jpeg(bytes, config); });
        // Tiny payload declaring a giant image must be rejected before imdecode.
        const std::vector<std::uint8_t> bomb{0xff,0xd8,0xff,0xc0,0,11,8,0xff,0xff,0xff,0xff,1,1,0x11,0,0xff,0xd9};
        rejects([&] { decode_jpeg(bomb, config); });
        std::cout << "configuration, algorithms and JPEG guards passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
