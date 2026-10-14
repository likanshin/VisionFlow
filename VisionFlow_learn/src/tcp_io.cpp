#include "tcp_io.hpp"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>


namespace visionflow::net {

SocketFd::SocketFd(int fd) : fd_(fd) {
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "获取套接字失败");
    }
}

SocketFd::SocketFd(SocketFd&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}


SocketFd::~SocketFd() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}


void recv_exact(int fd, std::uint8_t* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const auto count = ::recv(fd, data + done, size - done, 0);
        if (count > 0) {
            done += static_cast<std::size_t>(count);
        } else if (count == 0) {
            throw std::runtime_error("连接已关闭，尚未收齐消息数据");
        } else if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(),
                                    "接收数据失败");
        }
    }
}

void send_all(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const auto count = ::send(fd, data + done, size - done, MSG_NOSIGNAL);
        if (count > 0) {
            done += static_cast<std::size_t>(count);
        } else if (count == 0) {
            throw std::runtime_error("发送数据未取得进展");
        } else if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(),
                                    "发送数据失败");
        }
    }
}

} // namespace visionflow::net
