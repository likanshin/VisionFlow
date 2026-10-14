#include "protocol.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace visionflow::protocol {
namespace {

// 从末尾向前填充，低 8 位写到靠后的位置，得到大端字节序。
void put(std::uint8_t* out, std::uint64_t number, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        out[count - i - 1] = static_cast<std::uint8_t>(number & 0xFFU);
        number >>= 8U;
    }
}

// 从高位字节开始，每读入一个字节，就把已有数字左移 8 位。
std::uint64_t get(const std::uint8_t* bytes, std::size_t count) {
    std::uint64_t number = 0;
    for (std::size_t i = 0; i < count; ++i) {
        number = (number << 8U) | bytes[i];
    }
    return number;
}

} // namespace

std::array<std::uint8_t, header_size> encode(const Header& header) {
    std::array<std::uint8_t, header_size> bytes{};
    std::copy_n("VFLW", 4, bytes.begin());
    put(bytes.data() + 4, 1, 2);
    put(bytes.data() + 6, static_cast<std::uint16_t>(header.type), 2);
    put(bytes.data() + 8, header.request_id, 8);
    put(bytes.data() + 16, header.payload_length, 4);
    // 数组已全部初始化为 0，偏移 20 到 23 的 flags 保持为 0。
    return bytes;
}

Header decode(const std::uint8_t* bytes, std::size_t size,
              std::uint32_t max_payload) {
    if (size != header_size || bytes == nullptr) {
        throw std::runtime_error("消息头必须为 24 字节，且数据不能为空");
    }
    if (!std::equal(bytes, bytes + 4, "VFLW")) {
        throw std::runtime_error("消息头标记必须为 VFLW");
    }
    if (get(bytes + 4, 2) != 1) {
        throw std::runtime_error("不支持此协议版本");
    }
    if (get(bytes + 20, 4) != 0) {
        throw std::runtime_error("当前协议的 flags 必须为 0");
    }
    const auto type = get(bytes + 6, 2);
    if (type < 1 || type > 3) {
        throw std::runtime_error("未知的消息类型");
    }
    const auto length = get(bytes + 16, 4);
    if (length == 0 || length > max_payload) {
        throw std::runtime_error("消息体长度必须大于 0，且不能超过允许上限");
    }
    return {static_cast<Type>(type), get(bytes + 8, 8),
            static_cast<std::uint32_t>(length)};
}

std::vector<std::uint8_t> packet(Type type, std::uint64_t request_id,
                               const std::vector<std::uint8_t>& body) {
    if (body.empty() || body.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("消息体不能为空，且长度不能超过 32 位无符号整数的范围");
    }
    const auto header = encode({type, request_id,
                               static_cast<std::uint32_t>(body.size())});
    std::vector<std::uint8_t> bytes;
    bytes.reserve(header_size + body.size());
    bytes.insert(bytes.end(), header.begin(), header.end());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

} // namespace visionflow::protocol
