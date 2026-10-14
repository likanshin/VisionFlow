// Manual dependency diagnostic, deliberately not a normal CTest target.
// On Ubuntu 20.04 OpenCV 4.2 + TBB, argument 1 reproduced a LeakSanitizer
// report; argument 0 disables the nested scheduler and exited without leaks.
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <cstdlib>
int main(int argc, char** argv) {
    cv::setNumThreads(argc > 1 ? std::atoi(argv[1]) : 0);
    cv::Mat image(100, 100, CV_8UC1, cv::Scalar(0)), output;
    cv::GaussianBlur(image, output, {5, 5}, 1.5);
}
