#pragma once

#include <cstddef>
#include <vector>
#include <opencv2/core.hpp>
#include <memory>
#include <string>


struct ProcessResult {
    int width = 0;
    int height = 0;
    int channels = 0;
    int edge_pixels = 0;
    int roi_edge_pixels = 0;
    std::size_t candidate_line_count = 0;
    std::size_t kept_line_count = 0;
    std::vector<cv::Vec4i> lines;

    // 保留教学中用于观察中间步骤的图像。
    cv::Mat edges;
    cv::Mat roi_mask;
    cv::Mat roi_edges;
    cv::Mat preview;
};

class IProcessor {
public:
    virtual ~IProcessor() = default;
    virtual ProcessResult process(const cv::Mat& image) = 0;
};

class LaneLinesProcessor : public IProcessor {
public:
    explicit LaneLinesProcessor(int max_lines = 3);
    ProcessResult process(const cv::Mat& image) override;

private:
    int max_lines_;
};



class EdgesProcessor : public IProcessor {
public:
    ProcessResult process(const cv::Mat& image) override;
};

// 输入为非空的 8 位 BGR 图像；max_lines 必须为 1..256。
ProcessResult process_lane_lines(const cv::Mat& image, int max_lines = 3);
std::unique_ptr<IProcessor> create_processor(const std::string& name, int max_lines = 3);

