#include "protocol.hpp"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace protocol = visionflow::protocol;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    try {
        const protocol::Header original{protocol::Type::image, 42, 5};
        const auto bytes = protocol::encode(original);
        const std::array<std::uint8_t, protocol::header_size> expected{
            0x56, 0x46, 0x4C, 0x57, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,
            0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00
        };
        require(bytes == expected, "编码结果与协议规定的字节不一致");
        const auto decoded = protocol::decode(expected.data(), expected.size());
        require(decoded.type == protocol::Type::image &&
                decoded.request_id == 42 && decoded.payload_length == 5,
                "解码得到的字段不正确");

        std::cout << "消息头字节：";
        for (const auto byte : bytes) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<unsigned int>(byte) << ' ';
        }
        std::cout << std::dec << '\n';
        std::cout << "请求编号：" << decoded.request_id << '\n';
        std::cout << "消息体长度：" << decoded.payload_length << '\n';

        // JSON 消息体就是文本的字节；本例只有 ASCII 字符，也是合法 UTF-8。
        const std::string text = R"({"status":"ok"})";
        const std::vector<std::uint8_t> body(text.begin(), text.end());
        const auto message = protocol::packet(protocol::Type::result, 42, body);
        const auto result_header = protocol::decode(message.data(), protocol::header_size);
        require(result_header.type == protocol::Type::result &&
                result_header.request_id == 42 &&
                result_header.payload_length == body.size(), "拼接后的消息头不正确");
        require(message.size() == protocol::header_size + body.size(),
                "整条消息的字节数不正确");
        require(std::equal(body.begin(), body.end(),
                           message.begin() + protocol::header_size),
                "拼接改变了消息体内容");
        std::cout << "整条结果消息长度：" << message.size() << '\n';

        bool rejected = false;
        try {
            // 头中声明长度为 5，但本次仅允许最多 4 字节。
            (void)protocol::decode(expected.data(), expected.size(), 4);
        } catch (const std::runtime_error& error) {
            rejected = true;
            std::cout << "已拒绝超限消息：" << error.what() << '\n';
        }
        require(rejected, "解码器没有拒绝超限消息");
        std::cout << "协议检查通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "协议检查失败：" << error.what() << '\n';
        return 1;
    }
}
