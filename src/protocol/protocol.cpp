#include "visionflow/protocol/protocol.hpp"
#include <algorithm>
#include <limits>

namespace visionflow::protocol {
namespace {
void put(std::uint8_t* out, std::uint64_t n, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        out[count - i - 1] = static_cast<std::uint8_t>(n & 255U);
        n >>= 8U;
    }
}
std::uint64_t get(const std::uint8_t* p, std::size_t count) {
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < count; ++i) n = (n << 8U) | p[i];
    return n;
}
}
std::array<std::uint8_t, header_size> encode(const Header& h) {
    std::array<std::uint8_t, header_size> out{};
    std::copy_n("VFLW", 4, out.begin());
    put(out.data() + 4, 1, 2);
    put(out.data() + 6, static_cast<std::uint16_t>(h.type), 2);
    put(out.data() + 8, h.request_id, 8);
    put(out.data() + 16, h.payload_length, 4);
    return out;
}
Header decode(const std::uint8_t* p, std::size_t n, std::uint32_t max_payload) {
    if (n != header_size) throw std::runtime_error("invalid header size");
    if (!std::equal(p, p + 4, "VFLW")) throw std::runtime_error("bad magic");
    if (get(p + 4, 2) != 1) throw std::runtime_error("unsupported version");
    if (get(p + 20, 4) != 0) throw std::runtime_error("unsupported flags");
    const auto type = get(p + 6, 2);
    if (type < 1 || type > 3) throw std::runtime_error("unknown message type");
    const auto length = get(p + 16, 4);
    if (length == 0 || length > max_payload) throw std::runtime_error("payload length outside limit");
    return {static_cast<Type>(type), get(p + 8, 8), static_cast<std::uint32_t>(length)};
}
std::vector<std::uint8_t> packet(Type type, std::uint64_t id, const std::vector<std::uint8_t>& body) {
    if (body.size() > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("payload too large");
    const auto h = encode({type, id, static_cast<std::uint32_t>(body.size())});
    std::vector<std::uint8_t> out;
    out.reserve(header_size + body.size());
    out.insert(out.end(), h.begin(), h.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
}
