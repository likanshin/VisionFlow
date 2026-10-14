#pragma once

#include <cstddef>
#include <vector>
#include <opencv2/core.hpp>

// 计算线段的长度
double line_length(const cv::Vec4i& segment);

// 对线段进行排序并限制数量
void rank_and_limit(std::vector<cv::Vec4i>& lines,
                    std::size_t max_lines);
