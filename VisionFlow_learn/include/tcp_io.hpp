#pragma once

#include <cstddef>
#include <cstdint>

namespace visionflow::net {

class SocketFd {
public:
    explicit SocketFd(int fd);
    ~SocketFd();
    // 移动构造函数，允许将 SocketFd 对象从一个实例移动到另一个实例，而不进行复制。
    SocketFd(SocketFd&& other) noexcept;

    SocketFd(const SocketFd&) = delete;
    SocketFd& operator=(const SocketFd&) = delete;

    int get() const { return fd_; }

private:
    int fd_;
};

// recv_exact 和 send_all 函数用于在套接字上接收和发送指定大小的数据，确保数据完整传输。
void recv_exact(int fd, std::uint8_t* data, std::size_t size);
void send_all(int fd, const std::uint8_t* data, std::size_t size);

} // namespace visionflow::net
