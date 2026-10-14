#include "processor.hpp"
#include "protocol.hpp"
#include "tcp_io.hpp"

#include <array>
#include <cerrno>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

namespace protocol = visionflow::protocol;
namespace net = visionflow::net;

void check_call(int result, const char* message) {
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), message);
    }
}

int main() {
    try {
        net::SocketFd listener(::socket(AF_INET, SOCK_STREAM, 0));
        const int reuse = 1;
        check_call(::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                               &reuse, sizeof(reuse)), "设置地址复用失败");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(19090);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check_call(::bind(listener.get(),
                          reinterpret_cast<const sockaddr*>(&address),
                          sizeof(address)), "绑定监听地址失败");
        check_call(::listen(listener.get(), 1), "开始监听失败");
        std::cout << "正在监听 127.0.0.1:19090，等待一次图片请求……"
                  << std::endl;

        int accepted;
        do {
            accepted = ::accept(listener.get(), nullptr, nullptr);
        } while (accepted < 0 && errno == EINTR);
        net::SocketFd connection(accepted);
        std::cout << "客户端已连接\n";

        std::array<std::uint8_t, protocol::header_size> header_bytes{};
        net::recv_exact(connection.get(), header_bytes.data(),
                        header_bytes.size());
        const auto header = protocol::decode(header_bytes.data(),
                                              header_bytes.size());
        if (header.type != protocol::Type::image) {
            throw std::runtime_error("服务端只接受图片请求");
        }
        std::vector<std::uint8_t> body(header.payload_length);
        net::recv_exact(connection.get(), body.data(), body.size());

        nlohmann::json response;
        auto response_type = protocol::Type::result;
        std::string error_code = "invalid_image";
        try {
            const cv::Mat image = cv::imdecode(body, cv::IMREAD_COLOR);
            if (image.empty()) {
                throw std::runtime_error("消息体无法解码为图片");
            }
            error_code = "processing_failed";
            auto processor = create_processor("lane_lines", 3);
            const auto result = processor->process(image);
            response = {
                {"request_id", header.request_id},
                {"status", "ok"},
                {"processor", "lane_lines"},
                {"width", result.width},
                {"height", result.height},
                {"channels", result.channels},
                {"edge_pixels", result.edge_pixels},
                {"lines", nlohmann::json::array()}
            };
            for (const auto& line : result.lines) {
                response["lines"].push_back(nlohmann::json::array({
                    line[0], line[1], line[2], line[3]
                }));
            }
        } catch (const std::exception& error) {
            response_type = protocol::Type::error;
            response = {
                {"request_id", header.request_id},
                {"status", "error"},
                {"code", error_code},
                {"message", "图片请求处理失败"}
            };
            std::cerr << "图片请求处理失败：" << error.what() << '\n';
        }

        const std::string json_text = response.dump();
        constexpr std::size_t max_response = 64U * 1024U;
        if (json_text.size() > max_response - protocol::header_size) {
            throw std::runtime_error("响应内容超过允许上限");
        }
        const std::vector<std::uint8_t> response_body(json_text.begin(),
                                                        json_text.end());
        const auto message = protocol::packet(response_type,
                                               header.request_id,
                                               response_body);
        net::send_all(connection.get(), message.data(), message.size());
        std::cout << "响应已发送，本次连接处理结束\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "服务端失败：" << error.what() << '\n';
        return 1;
    }
}
