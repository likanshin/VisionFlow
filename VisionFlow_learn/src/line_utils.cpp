#include "line_utils.hpp"

#include <algorithm>
#include <cmath>


double line_length(const cv::Vec4i& segment) {
    const double dx = segment[2] - segment[0];
    const double dy = segment[3] - segment[1];
    return std::hypot(dx, dy);
}

void rank_and_limit(std::vector<cv::Vec4i>& lines,
                    std::size_t max_lines) {
    std::sort(lines.begin(), lines.end(),
              [](const cv::Vec4i& a, const cv::Vec4i& b) {
                  return line_length(a) > line_length(b);
              });

    if (lines.size() > max_lines) {
        lines.resize(max_lines);
    }
}