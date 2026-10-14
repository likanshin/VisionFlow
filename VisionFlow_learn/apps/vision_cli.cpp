#include "line_utils.hpp"
#include "processor.hpp"

#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <exception>
#include <string>
#include <stdexcept>
#include <fstream>
#include <nlohmann/json.hpp>
#include <chrono>

class Logger {
public:
    void set_level(const std::string& level) {
        if (level != "info" && level != "error") {
            throw std::invalid_argument("log_level 必须为 info 或 error");
        }
        errors_only_ = (level == "error");
    }

    void info(const std::string& message,
              const nlohmann::json& context = nlohmann::json::object()) const {
        if (!errors_only_) {
            write("info", message, context);
        }
    }

    void error(const std::string& message,
               const nlohmann::json& context = nlohmann::json::object()) const {
        write("error", message, context);
    }

private:
    bool errors_only_ = false;

    void write(const std::string& level, const std::string& message,
               const nlohmann::json& context) const {
        const auto now = std::chrono::system_clock::now();
        const auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        nlohmann::json record = nlohmann::json::object();
        record["time_ms"] = time_ms;
        record["level"] = level;
        record["message"] = message;
        record["context"] = context;

        std::cerr << record.dump() << '\n';
    }
};


struct Config {
    int max_lines = 3;
    std::string log_level = "info";
};

Config load_config(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("无法打开配置文件：" + path);
    }

    const nlohmann::json document = nlohmann::json::parse(file);
    if (!document.is_object()) {
        throw std::invalid_argument("配置文件的顶层必须是 JSON 对象");
    }

    for (auto it = document.begin(); it != document.end(); ++it) {
        if (it.key() != "max_lines" && it.key() != "log_level") {
            throw std::invalid_argument("未知的配置字段：" + it.key());
        }
    }

    Config config;
    // 解析 max_lines 配置项，确保其为整数且在 1 到 256 的范围内。
    const auto it = document.find("max_lines");
    if (it != document.end()) {
        const auto& value = it.value();
        if (!value.is_number_integer()) {
            throw std::invalid_argument("max_lines 必须为整数");
        }

        const double number = value.get<double>();
        if (number < 1 || number > 256) {
            throw std::invalid_argument("max_lines 必须在 1 到 256 之间");
        }
        config.max_lines = value.get<int>();
    }

    // 解析 log_level 配置项，确保其为字符串且值为 "info" 或 "error"。
    const auto level_it = document.find("log_level");
    if (level_it != document.end()) {
        if (!level_it.value().is_string()) {
            throw std::invalid_argument("log_level 必须为字符串");
        }
        const std::string level = level_it.value().get<std::string>();
        if (level != "info" && level != "error") {
            throw std::invalid_argument("log_level 必须为 info 或 error");
        }
        config.log_level = level;
    }

    return config;
}



int main(int argc, char* argv[]) {
    Logger logger;
    std::string stage = "arguments";

    try {
        std::string input_path = "samples/frame.jpg";
        std::string processor_name = "edges";
        std::string config_path;

        // 解析命令行参数。
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];

            if (arg == "--help") {
                std::cout
                    << "用法：vision_cli [--input 路径] [--processor 名称] [--config 路径] [--help]\n"
                    << "  --input 路径      输入图片（默认：samples/frame.jpg）\n"
                    << "  --processor 名称  处理器：edges（边缘检测）或 lane_lines（道路线段检测），默认：edges\n"
                    << "  --config 路径     JSON 配置文件（默认：max_lines=3，log_level=info）\n"
                    << "  --help            显示帮助并退出\n";
                return 0;
            }

            if (arg == "--input" || arg == "--processor" || arg == "--config") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("以下参数缺少值：" + arg);
                }

                const std::string value = argv[i + 1];
                if (value.empty() ||
                    (value.size() >= 2 && value[0] == '-' && value[1] == '-')) {
                    throw std::invalid_argument("以下参数缺少值：" + arg);
                }

                ++i;
                if (arg == "--input") {
                    input_path = value;
                } else if (arg == "--processor") {
                    processor_name = value;
                } else {
                    config_path = value;
                }
            } else {
                throw std::invalid_argument("未知的命令行参数：" + arg);
            }
        }

        // 加载配置文件，如果指定了路径。
        stage = "config";
        Config config;
        if (!config_path.empty()) {
            config = load_config(config_path);
        }
        logger.set_level(config.log_level);

        logger.info("开始处理", {
            {"input", input_path},
            {"processor", processor_name},
            {"max_lines", config.max_lines}
        });


        std::cout << "我正在重新构建 VisionFlow。\n";

        // 相对路径从运行程序时的工作目录开始计算。
        stage = "read_image";
        cv::Mat image = cv::imread(input_path, cv::IMREAD_COLOR);
        if (image.empty()) {
            throw std::runtime_error("无法读取图片：" + input_path);
        }


        stage = "create_processor";
        auto processor = create_processor(processor_name, config.max_lines);
        stage = "process_image";
        const ProcessResult result = processor->process(image);

        std::cout << "宽度：" << result.width << "\n";
        std::cout << "高度：" << result.height << "\n";
        std::cout << "通道数：" << result.channels << "\n";
        std::cout << "边缘像素数：" << result.edge_pixels << "\n";

        if (!result.roi_edges.empty()) {
            std::cout << "感兴趣区域内的边缘像素数：" << result.roi_edge_pixels << "\n";
            std::cout << "检测到的线段数：" << result.candidate_line_count << "\n";
            std::cout << "方向筛选后保留的线段数：" << result.kept_line_count << "\n";
        }
        std::cout << "最终选取的线段数：" << result.lines.size() << "\n";

        for (const auto& segment : result.lines) {
            std::cout << "(" << segment[0] << ", " << segment[1] << ") -> ("
                      << segment[2] << ", " << segment[3] << ")"
                      << " 长度=" << line_length(segment) << "\n";
        }

        // PNG 无损保存边缘图，保留 0/255 两种像素值。
        stage = "save_outputs";
        if (!cv::imwrite("build/edges.png", result.edges)) {
            throw std::runtime_error("无法保存图片：build/edges.png");
        }

        std::cout << "已保存：build/edges.png\n";

        if (!result.roi_mask.empty()) {
            if (!cv::imwrite("build/roi_mask.png", result.roi_mask)) {
                throw std::runtime_error("无法保存图片：build/roi_mask.png");
            }
            std::cout << "已保存：build/roi_mask.png\n";
        }
        if (!result.roi_edges.empty()) {
            if (!cv::imwrite("build/roi_edges.png", result.roi_edges)) {
                throw std::runtime_error("无法保存图片：build/roi_edges.png");
            }
            std::cout << "已保存：build/roi_edges.png\n";
        }


        if (!cv::imwrite("build/preview.png", result.preview)) {
            throw std::runtime_error("无法保存图片：build/preview.png");
        }
        std::cout << "已保存：build/preview.png\n";

        // 保存结果为 JSON 文件，便于后续分析。
        nlohmann::json document = nlohmann::json::object();
        document["status"] = "ok";
        document["processor"] = processor_name;
        document["max_lines"] = config.max_lines;
        document["width"] = result.width;
        document["height"] = result.height;
        document["channels"] = result.channels;
        document["edge_pixels"] = result.edge_pixels;
        document["lines"] = nlohmann::json::array();

        for (const auto& segment : result.lines) {
            document["lines"].push_back(nlohmann::json::array({
                segment[0], segment[1], segment[2], segment[3]
            }));
        }

        const std::string json_text = document.dump(2);
        const std::string output_path = "build/result.json";
        std::ofstream output(output_path);
        if (!output) {
            throw std::runtime_error("无法打开输出文件：" + output_path);
        }

        output << json_text << '\n';
        output.close();
        if (!output) {
            throw std::runtime_error("无法写入输出文件：" + output_path);
        }

        std::cout << "已保存：" << output_path << "\n";

        // 记录处理完成的日志信息
        logger.info("处理完成，结果已保存", {
            {"processor", processor_name},
            {"line_count", result.lines.size()},
            {"result_path", output_path}
        });

        return 0;
    } catch (const cv::Exception& error) {
        logger.error("OpenCV 操作失败", {
            {"stage", stage},
            {"error", error.what()}
        });
        return 1;
    } catch (const std::exception& error) {
        logger.error("处理失败", {
            {"stage", stage},
            {"error", error.what()}
        });
        return 1;
    }

}
