#include "processor.hpp"
#include "image_worker_pool.hpp"
#include "protocol.hpp"
#include "tcp_io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <csignal>
#include <optional>
#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace protocol = visionflow::protocol;
namespace net = visionflow::net;
namespace jobs = visionflow::jobs;

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t io_chunk = 64U * 1024U;
constexpr std::size_t max_connections = 8;
constexpr auto idle_timeout = std::chrono::seconds(10);
constexpr int wait_timeout_ms = 200;
constexpr auto processing_timeout = std::chrono::seconds(30);

// 处理结果交回后，等待发送完成的超时时间由内核 TCP 超时控制。
constexpr std::uint64_t listener_token = 0;
// 用于监听套接字的标识，避免使用 0/1 以免与工作线程冲突。
constexpr std::uint64_t completion_token = 1;
// 用于通知事件循环退出的标识，避免使用 0/1 以免与监听器/工作线程冲突。
constexpr std::uint64_t stop_token = 2;


enum class Phase { header, body, waiting_result, sending };

// 每个连接的状态信息
struct Connection {
    explicit Connection(net::SocketFd&& accepted)
        : socket(std::move(accepted)) {}

    net::SocketFd socket;
    Phase phase = Phase::header;
    std::array<std::uint8_t, protocol::header_size> header_bytes{};
    std::size_t header_done = 0;
    protocol::Header header{};
    std::vector<std::uint8_t> body;
    std::size_t body_done = 0;
    std::vector<std::uint8_t> output;
    std::size_t sent = 0;
    Clock::time_point last_progress = Clock::now();
    Clock::time_point submitted_at{};
};

using Connections = std::unordered_map<std::uint64_t, Connection>;

void check_call(int result, const char* message) {
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), message);
    }
}

bool would_block(int error) {
    return error == EAGAIN || error == EWOULDBLOCK;
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    check_call(flags, "读取套接字状态失败");
    check_call(::fcntl(fd, F_SETFL, flags | O_NONBLOCK),
               "设置非阻塞模式失败");
}

void set_interest(int epoll_fd, int operation, int fd,
                  std::uint64_t token, std::uint32_t interests) {
    epoll_event event{};
    event.events = interests;
    event.data.u64 = token;
    check_call(::epoll_ctl(epoll_fd, operation, fd, &event),
               "更新 epoll 关注事件失败");
}

std::vector<std::uint8_t> make_response(
    const protocol::Header& header,
    const std::vector<std::uint8_t>& body) {
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
    } catch (const std::exception&) {
        response_type = protocol::Type::error;
        response = {
            {"request_id", header.request_id},
            {"status", "error"},
            {"code", error_code},
            {"message", "图片请求处理失败"}
        };
        // 工作线程只生成响应；连接日志统一由事件循环输出。
    }

    const std::string text = response.dump();
    constexpr std::size_t max_response = 64U * 1024U;
    if (text.size() > max_response - protocol::header_size) {
        throw std::runtime_error("响应内容超过允许上限");
    }
    const std::vector<std::uint8_t> response_body(text.begin(), text.end());
    return protocol::packet(response_type, header.request_id, response_body);
}

std::vector<std::uint8_t> make_busy_response(const protocol::Header& header) {
    const nlohmann::json response = {
        {"request_id", header.request_id},
        {"status", "error"},
        {"code", "busy"},
        {"message", "处理任务已满，请稍后重试"}
    };
    const auto text = response.dump();
    return protocol::packet(protocol::Type::error, header.request_id,
                            std::vector<std::uint8_t>(text.begin(), text.end()));
}

void start_sending(int epoll_fd, std::uint64_t token, Connection& connection,
                   std::vector<std::uint8_t> output) {
    connection.output = std::move(output);
    connection.sent = 0;
    connection.phase = Phase::sending;
    connection.last_progress = Clock::now();
    set_interest(epoll_fd, EPOLL_CTL_MOD, connection.socket.get(), token,
                 EPOLLOUT);
}

void close_connection(int epoll_fd, Connections& connections,
                      std::uint64_t token, const char* reason) {
    const auto it = connections.find(token);
    if (it == connections.end()) {
        return;
    }
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_DEL,
                    it->second.socket.get(), nullptr) < 0) {
        std::cerr << "移除 epoll 关注失败："
                  << std::system_error(errno, std::generic_category()).what()
                  << '\n';
    }
    connections.erase(it);
    std::cout << "连接 " << token << " 已关闭：" << reason << std::endl;
}

void accept_ready(int epoll_fd, int listener_fd,
                  Connections& connections, std::uint64_t& next_token) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const int fd = ::accept(listener_fd, nullptr, nullptr);
        if (fd < 0) {
            const int error = errno;
            if (would_block(error)) {
                return;
            }
            if (error == EINTR || error == ECONNABORTED) {
                continue;
            }
            throw std::system_error(error, std::generic_category(),
                                    "接受客户端连接失败");
        }
        net::SocketFd accepted(fd);
        if (connections.size() >= max_connections) {
            std::cerr << "连接数已达上限，关闭新连接\n";
            continue;
        }

        if (next_token == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("连接标识已耗尽");
        }
        const auto token = next_token++;
        try {
            set_nonblocking(accepted.get());
            const auto inserted =
                connections.try_emplace(token, std::move(accepted));
            try {
                set_interest(epoll_fd, EPOLL_CTL_ADD,
                             inserted.first->second.socket.get(), token,
                             EPOLLIN | EPOLLRDHUP);
            } catch (...) {
                connections.erase(inserted.first);
                throw;
            }
            std::cout << "接受连接 " << token
                      << "，当前连接数：" << connections.size() << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "初始化连接失败：" << error.what() << '\n';
        }
    }
}

void read_step(int epoll_fd, std::uint64_t token, Connection& connection,
               jobs::ImageWorkerPool& pool) {
    std::uint8_t* destination;
    std::size_t remaining;
    if (connection.phase == Phase::header) {
        destination = connection.header_bytes.data() + connection.header_done;
        remaining = connection.header_bytes.size() - connection.header_done;
    } else {
        destination = connection.body.data() + connection.body_done;
        remaining = connection.body.size() - connection.body_done;
    }

    const auto count = ::recv(connection.socket.get(), destination,
                              std::min(remaining, io_chunk), 0);
    if (count < 0) {
        const int error = errno;
        if (would_block(error) || error == EINTR) {
            return;
        }
        throw std::system_error(error, std::generic_category(),
                                "接收数据失败");
    }
    if (count == 0) {
        throw std::runtime_error("对端结束发送，请求尚未收齐");
    }
    connection.last_progress = Clock::now();
    const auto received = static_cast<std::size_t>(count);

    if (connection.phase == Phase::header) {
        connection.header_done += received;
        std::cout << "连接 " << token << "：消息头进度 "
                  << connection.header_done << "/"
                  << connection.header_bytes.size() << std::endl;
        if (connection.header_done == connection.header_bytes.size()) {
            connection.header = protocol::decode(
                connection.header_bytes.data(), connection.header_bytes.size());
            if (connection.header.type != protocol::Type::image) {
                throw std::runtime_error("服务端只接受图片请求");
            }
            connection.body.resize(connection.header.payload_length);
            connection.phase = Phase::body;
        }
        return;
    }

    connection.body_done += received;
    if (connection.body_done == connection.body.size()) {
        jobs::ImageTask task{
            token, connection.header, std::move(connection.body)
        };
        const auto result = pool.try_submit(std::move(task));
        if (result == jobs::SubmitResult::accepted) {
            connection.phase = Phase::waiting_result;
            connection.submitted_at = Clock::now();
            set_interest(epoll_fd, EPOLL_CTL_MOD, connection.socket.get(), token,
                         EPOLLRDHUP);
            std::cout << "连接 " << token << "：请求 "
                      << connection.header.request_id << " 已提交工作线程"
                      << std::endl;
        } else {
            start_sending(epoll_fd, token, connection,
                          make_busy_response(connection.header));
            std::cout << "连接 " << token << "：请求 "
                      << connection.header.request_id << " 被拒绝，返回 busy"
                      << std::endl;
        }
    }
}

bool write_step(Connection& connection) {
    const auto remaining = connection.output.size() - connection.sent;
    const auto count = ::send(
        connection.socket.get(),
        connection.output.data() + connection.sent,
        std::min(remaining, io_chunk), MSG_NOSIGNAL);
    if (count < 0) {
        const int error = errno;
        if (would_block(error) || error == EINTR) {
            return false;
        }
        throw std::system_error(error, std::generic_category(),
                                "发送数据失败");
    }
    if (count == 0) {
        throw std::runtime_error("发送数据未取得进展");
    }
    connection.sent += static_cast<std::size_t>(count);
    connection.last_progress = Clock::now();
    return connection.sent == connection.output.size();
}

void apply_completed(int epoll_fd, Connections& connections,
                     jobs::ImageWorkerPool& pool) {
    auto results = pool.take_completed();
    for (auto& result : results) {
        const auto token = result.connection_token;
        const auto it = connections.find(token);
        if (it == connections.end() ||
            it->second.phase != Phase::waiting_result) {
            std::cout << "丢弃连接 " << token << " 的过期结果" << std::endl;
            continue;
        }
        try {
            if (result.error) {
                std::rethrow_exception(result.error);
            }
            start_sending(epoll_fd, token, it->second, std::move(result.output));
            std::cout << "连接 " << token << "：工作线程 "
                      << result.worker_index << " 已交回结果" << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "连接 " << token
                      << " 的结果处理失败：" << error.what() << '\n';
            close_connection(epoll_fd, connections, token, "结果处理失败");
        } catch (...) {
            close_connection(epoll_fd, connections, token, "结果发生未知异常");
        }
    }
}

void expire_connections(int epoll_fd, Connections& connections) {
    const auto now = Clock::now();
    for (auto it = connections.begin(); it != connections.end();) {
        const auto& connection = it->second;
        const bool processing = connection.phase == Phase::waiting_result;
        const bool expired = processing
            ? now - connection.submitted_at >= processing_timeout
            : now - connection.last_progress >= idle_timeout;
        if (expired) {
            const auto token = it->first;
            ++it;
            close_connection(epoll_fd, connections, token,
                             processing ? "等待处理结果超过 30 秒"
                                        : "10 秒内没有收发进展");
        } else {
            ++it;
        }
    }
}

std::chrono::milliseconds parse_demo_delay(int argc, char* argv[]) {
    if (argc == 1) {
        return std::chrono::milliseconds{0};
    }
    if (argc != 3 || std::string(argv[1]) != "--demo-delay-ms") {
        throw std::invalid_argument(
            "用法：vision_pool_server [--demo-delay-ms 0到5000]");
    }
    const std::string text = argv[2];
    if (text.empty() || text.size() > 4 ||
        !std::all_of(text.begin(), text.end(),
                     [](char c) { return c >= '0' && c <= '9'; })) {
        throw std::invalid_argument("观察延迟必须为 0～5000 的整数");
    }
    const int value = std::stoi(text);
    if (value > 5000) {
        throw std::invalid_argument("观察延迟不得超过 5000 毫秒");
    }
    return std::chrono::milliseconds{value};
}

// 读取信号描述符，返回观察到的 SIGINT/SIGTERM 信号编号，或 0 表示未观察到。
int take_stop_signal(int signal_fd) {
    int observed = 0;
    for (int attempt = 0; attempt < 16; ++attempt) {
        signalfd_siginfo info{};
        const auto count = ::read(signal_fd, &info, sizeof(info));
        if (count == static_cast<ssize_t>(sizeof(info))) {
            if (observed == 0 &&
                (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM)) {
                observed = static_cast<int>(info.ssi_signo);
            }
            continue;
        }
        const int error = errno;
        if (count < 0 && error == EINTR) {
            continue;
        }
        if (count < 0 && would_block(error)) {
            return observed;
        }
        if (count < 0) {
            throw std::system_error(error, std::generic_category(),
                                    "读取停止信号失败");
        }
        throw std::runtime_error("停止信号记录长度不正确");
    }
    return observed;
}

// 处理 SIGINT/SIGTERM 信号，开始停止服务端。
void begin_stop(int signo, int epoll_fd,
                std::optional<net::SocketFd>& listener,
                Connections& connections, jobs::ImageWorkerPool& pool,
                bool& stopping) {
    if (stopping) {
        std::cout << "已经在停止，继续等待已有任务和响应" << std::endl;
        return;
    }
    stopping = true;
    std::cout << "收到 " << (signo == SIGINT ? "SIGINT" : "SIGTERM")
              << "，开始停止；未交回任务：" << pool.outstanding()
              << "，当前连接数：" << connections.size() << std::endl;

    if (listener) {
        check_call(::epoll_ctl(epoll_fd, EPOLL_CTL_DEL,
                               listener->get(), nullptr),
                   "移除监听关注失败");
        listener.reset();
    }
    pool.stop_accepting();

    for (auto it = connections.begin(); it != connections.end();) {
        const auto token = it->first;
        const auto phase = it->second.phase;
        ++it;
        if (phase == Phase::header || phase == Phase::body) {
            close_connection(epoll_fd, connections, token,
                             "服务端停止，请求尚未提交");
        }
    }
    std::cout << "停止接收新连接和新任务，继续处理已有任务与响应"
              << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const auto demo_delay = parse_demo_delay(argc, argv);
        sigset_t stop_mask;
        check_call(::sigemptyset(&stop_mask), "初始化信号集合失败");
        check_call(::sigaddset(&stop_mask, SIGINT), "添加 SIGINT 失败");
        check_call(::sigaddset(&stop_mask, SIGTERM), "添加 SIGTERM 失败");
        const int mask_error = ::pthread_sigmask(SIG_BLOCK, &stop_mask, nullptr);
        if (mask_error != 0) {
            throw std::system_error(mask_error, std::generic_category(),
                                    "阻塞停止信号失败");
        }
        const int stop_fd = ::signalfd(
            -1, &stop_mask, SFD_NONBLOCK | SFD_CLOEXEC);
        check_call(stop_fd, "创建停止信号描述符失败");
        net::SocketFd signals(stop_fd);

        std::optional<net::SocketFd> listener(
            std::in_place, ::socket(AF_INET, SOCK_STREAM, 0));
        const int reuse = 1;
        check_call(::setsockopt(listener->get(), SOL_SOCKET, SO_REUSEADDR,
                               &reuse, sizeof(reuse)), "设置地址复用失败");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(19090);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check_call(::bind(listener->get(),
                          reinterpret_cast<const sockaddr*>(&address),
                          sizeof(address)), "绑定监听地址失败");
        check_call(::listen(listener->get(), 128), "开始监听失败");
        set_nonblocking(listener->get());

        const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        check_call(epoll_fd, "创建 epoll 实例失败");
        net::SocketFd poller(epoll_fd);
        set_interest(poller.get(), EPOLL_CTL_ADD, listener->get(),
                     listener_token, EPOLLIN);

        set_interest(poller.get(), EPOLL_CTL_ADD, signals.get(),
                     stop_token, EPOLLIN);

        {
            Connections connections;
            jobs::ImageWorkerPool pool(make_response, demo_delay);
            set_interest(poller.get(), EPOLL_CTL_ADD, pool.notification_fd(),
                         completion_token, EPOLLIN);
            std::uint64_t next_token = 3;
            bool stopping = false;
            std::array<epoll_event, 32> events{};
            std::cout << "服务端 PID：" << ::getpid() << std::endl;
            std::cout << "线程池服务端正在监听 127.0.0.1:19090；工作线程 2，"
                         "排队容量 2，未交回任务上限 4；观察延迟 "
                      << demo_delay.count() << " 毫秒" << std::endl;

            while (!stopping || pool.outstanding() != 0 || !connections.empty()) {
                int ready = ::epoll_wait(
                    poller.get(), events.data(), static_cast<int>(events.size()),
                    wait_timeout_ms);
                if (ready < 0) {
                    if (errno == EINTR) {
                        ready = 0;
                    } else {
                        check_call(ready, "等待 epoll 事件失败");
                    }
                }
                // 先处理停止通知，再处理本批网络事件。
                for (int i = 0; i < ready; ++i) {
                    if (events[i].data.u64 == stop_token &&
                        (events[i].events & (EPOLLERR | EPOLLHUP))) {
                        throw std::runtime_error("停止信号描述符发生错误");
                    }
                }
                const int signo = take_stop_signal(signals.get());
                if (signo != 0) {
                    begin_stop(signo, poller.get(), listener,
                               connections, pool, stopping);
                }

                for (int i = 0; i < ready; ++i) {
                    const auto token = events[i].data.u64;
                    const auto flags = events[i].events;
                    if (token == stop_token) {
                        continue; // 本批开始时已经读取停止通知。
                    }
                    if (token == listener_token) {
                        if (stopping || !listener) {
                            continue; // 忽略本批中已经过期的监听事件。
                        }
                        if (flags & (EPOLLERR | EPOLLHUP)) {
                            throw std::runtime_error("监听套接字发生错误");
                        }
                        accept_ready(poller.get(), listener->get(),
                                     connections, next_token);
                        continue;
                    }
                    if (token == completion_token) {
                        if (flags & (EPOLLERR | EPOLLHUP)) {
                            throw std::runtime_error("完成通知描述符发生错误");
                        }
                        pool.drain_notifications();
                        apply_completed(poller.get(), connections, pool);
                        continue;
                    }
                    const auto it = connections.find(token);
                    if (it == connections.end()) {
                        continue;
                    }
                    try {
                        auto& connection = it->second;
                        if (flags & EPOLLERR) {
                            throw std::runtime_error("连接发生网络错误");
                        }
                        if ((connection.phase == Phase::header ||
                             connection.phase == Phase::body) &&
                            (flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP))) {
                            read_step(poller.get(), token, connection, pool);
                        }
                        if (connection.phase == Phase::waiting_result) {
                            if (flags & EPOLLHUP) {
                                throw std::runtime_error("等待结果期间连接挂断");
                            }
                            if (flags & EPOLLRDHUP) {
                                // 已收齐请求；对端可能只是关闭发送方向。
                                // 关闭这一持续通知，仍会收到 EPOLLERR/EPOLLHUP。
                                set_interest(poller.get(), EPOLL_CTL_MOD,
                                             connection.socket.get(), token, 0);
                            }
                        }
                        if (connection.phase == Phase::sending &&
                            (flags & (EPOLLOUT | EPOLLHUP))) {
                            if (write_step(connection)) {
                                close_connection(poller.get(), connections, token,
                                                 "响应已发送");
                            } else if (flags & EPOLLHUP) {
                                throw std::runtime_error("对端挂断，响应未发完");
                            }
                        }
                    } catch (const std::exception& error) {
                        std::cerr << "连接 " << token
                                  << " 处理失败：" << error.what() << '\n';
                        close_connection(poller.get(), connections, token,
                                         "本次连接失败");
                    }
                }
                pool.check_health();
                expire_connections(poller.get(), connections);
            }
        } // pool 析构：close 幂等调用，并 join 所有工作线程。

        std::cout << "任务和连接已处理完毕，工作线程已回收，服务端正常退出"
                  << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "线程池服务端失败：" << error.what() << '\n';
        return 1;
    }
}

