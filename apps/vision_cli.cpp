#include "visionflow/vision/processor.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
int main(int argc, char** argv) {
    try {
        std::string input, output, preview, processor = "lane_lines";
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::cout << "vision_cli --input image.jpg --output result.json [--processor edges|lane_lines] [--preview result.jpg]\n";
                return 0;
            }
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            const std::string value = argv[++i];
            if (key == "--input") input = value;
            else if (key == "--output") output = value;
            else if (key == "--processor") processor = value;
            else if (key == "--preview") preview = value;
            else throw std::invalid_argument("unknown option: " + key);
        }
        if (input.empty() || output.empty()) throw std::invalid_argument("--input and --output are required");
        visionflow::Config config;
        const auto size = std::filesystem::file_size(input);
        if (size > config.max_payload_bytes) throw std::invalid_argument("input exceeds payload limit");
        std::ifstream file(input, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open input: " + input);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) throw std::runtime_error("cannot read input");
        cv::setNumThreads(0); // Disable OpenCV's nested parallel backend entirely.
        const auto start = std::chrono::steady_clock::now();
        const auto image = visionflow::decode_jpeg(bytes, config);
        auto worker = visionflow::create_processor(processor);
        const auto result = worker->process({image, 1});
        auto json = result.json(1);
        json["total_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const auto parent = std::filesystem::path(output).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        std::ofstream out(output);
        if (!(out << json.dump(2) << '\n')) throw std::runtime_error("cannot write result");
        if (!preview.empty()) {
            const auto dir = std::filesystem::path(preview).parent_path();
            if (!dir.empty()) std::filesystem::create_directories(dir);
            if (!cv::imwrite(preview, result.preview)) throw std::runtime_error("cannot write preview");
        }
        std::cout << json.dump() << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "vision_cli: " << e.what() << '\n'; return 1; }
}
