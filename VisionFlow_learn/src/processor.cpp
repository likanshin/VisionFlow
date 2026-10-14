#include "processor.hpp"
#include "line_utils.hpp"

#include <cmath>
#include <opencv2/imgproc.hpp>
#include <stdexcept>

// LaneLinesProcessor 构造函数，设置最大线段数量，并检查其范围。
LaneLinesProcessor::LaneLinesProcessor(int max_lines)
    : max_lines_(max_lines) {
    if (max_lines_ < 1 || max_lines_ > 256) {
        throw std::invalid_argument("max_lines 必须在 1 到 256 之间");
    }
}


// LaneLinesProcessor 的实现类。
ProcessResult LaneLinesProcessor::process(const cv::Mat& image) {
    return process_lane_lines(image, max_lines_);
}


// EdgesProcessor 的实现类。
ProcessResult EdgesProcessor::process(const cv::Mat& image) {
    ProcessResult result;
    result.width = image.cols;
    result.height = image.rows;
    result.channels = image.channels();

    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);
    cv::Canny(blurred, result.edges, 100, 200);
    result.edge_pixels = cv::countNonZero(result.edges);

    cv::cvtColor(result.edges, result.preview, cv::COLOR_GRAY2BGR);
    return result;
}

ProcessResult process_lane_lines(const cv::Mat& image, int max_lines) {
    if (max_lines < 1 || max_lines > 256) {
        throw std::invalid_argument("max_lines 必须在 1 到 256 之间");
    }

    ProcessResult result;
    result.width = image.cols;
    result.height = image.rows;
    result.channels = image.channels();

    // 提取全图边缘。
    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);
    cv::Canny(blurred, result.edges, 100, 200);
    result.edge_pixels = cv::countNonZero(result.edges);

    // 保留道路 ROI 内的边缘，图像坐标保持不变。
    const int w = result.width;
    const int h = result.height;
    result.roi_mask = cv::Mat::zeros(result.edges.size(), CV_8UC1);
    std::vector<cv::Point> polygon{
        {w / 10, h - 1},
        {w * 45 / 100, h * 55 / 100},
        {w * 55 / 100, h * 55 / 100},
        {w * 9 / 10, h - 1}
    };
    cv::fillConvexPoly(result.roi_mask, polygon, cv::Scalar(255));
    cv::bitwise_and(result.edges, result.roi_mask, result.roi_edges);
    result.roi_edge_pixels = cv::countNonZero(result.roi_edges);

    cv::Mat hough_input = result.roi_edges.clone();
    std::vector<cv::Vec4i> candidates;
    cv::HoughLinesP(hough_input, candidates, 1, CV_PI / 180, 25, 30, 20);
    result.candidate_line_count = candidates.size();

    // 排除零长度和接近水平的线段。
    const double min_abs_slope = 0.35;
    for (const auto& segment : candidates) {
        const double dx = segment[2] - segment[0];
        const double dy = segment[3] - segment[1];
        if (dx == 0 && dy == 0) {
            continue;
        }
        if (std::abs(dy) < std::abs(dx) * min_abs_slope) {
            continue;
        }
        result.lines.push_back(segment);
    }
    result.kept_line_count = result.lines.size();

    rank_and_limit(result.lines, static_cast<std::size_t>(max_lines));


    // 绘制在独立副本上，不改动输入图像。
    result.preview = image.clone();
    for (const auto& segment : result.lines) {
        cv::line(result.preview,
                 cv::Point(segment[0], segment[1]),
                 cv::Point(segment[2], segment[3]),
                 cv::Scalar(0, 255, 0), 2);
    }
    return result;
}
// 根据字符串名称来决定返回哪种处理器的实例。返回值为智能指针，确保资源管理。
std::unique_ptr<IProcessor> create_processor(const std::string& name,
                                             int max_lines) {
    if (max_lines < 1 || max_lines > 256) {
        throw std::invalid_argument("max_lines 必须在 1 到 256 之间");
    }
    if (name == "edges") {
        return std::make_unique<EdgesProcessor>();
    }
    if (name == "lane_lines") {
        return std::make_unique<LaneLinesProcessor>(max_lines);
    }
    throw std::invalid_argument("未知的处理器：" + name);
}


