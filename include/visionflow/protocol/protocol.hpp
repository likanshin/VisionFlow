#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace visionflow::protocol {
constexpr std::size_t header_size = 24;
enum class Type : std::uint16_t { image = 1, result = 2, error = 3 };
struct Header {
    Type type = Type::image;
    std::uint64_t request_id = 0;
    std::uint32_t payload_length = 0;
};
std::array<std::uint8_t, header_size> encode(const Header& header);
Header decode(const std::uint8_t* bytes, std::size_t size, std::uint32_t max_payload);
std::vector<std::uint8_t> packet(Type type, std::uint64_t id, const std::vector<std::uint8_t>& body);
} // namespace visionflow::protocol
