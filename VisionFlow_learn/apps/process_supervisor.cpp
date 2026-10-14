#include "tcp_io.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace net = visionflow::net;

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kGrace = std::chrono::milliseconds(3000);
constexpr int kTickMs = 100;

void check_call(int result, const char* message) {
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), message);
    }
}

void set_cloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    check_call(flags, "读取描述符标志失败");
    check_call(::fcntl(fd, F_SETFD, flags | FD_CLOEXEC),
               "设置执行时关闭失败");
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    check_call(flags, "读取文件状态标志失败");
    check_call(::fcntl(fd, F_SETFL, flags | O_NONBLOCK),
               "设置非阻塞失败");
}

enum class LaunchStage : int { group = 1, signals = 2, exec = 3 };
struct LaunchError {
    LaunchStage stage;
    int code;
};

const char* stage_name(LaunchStage stage) {
    switch (stage) {
    case LaunchStage::group: return "进程组设置";
    case LaunchStage::signals: return "信号设置";
    case LaunchStage::exec: return "execv";
    }
    return "未知阶段";
}

[[noreturn]] void child_failed(int fd, LaunchStage stage, int code) {
    const LaunchError report{stage, code};
    const auto* data = reinterpret_cast<const char*>(&report);
    std::size_t sent = 0;
    while (sent < sizeof(report)) {
        const auto n = ::write(fd, data + sent, sizeof(report) - sent);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    ::_exit(127);
}

// 保存非阻塞错误管道的读取进度；对象不拥有传入的 fd。
class LaunchReader {
public:
    void pump(int fd) {
        if (done_) return;
        auto* data = reinterpret_cast<char*>(&report_);
        while (received_ < sizeof(report_)) {
            const auto n = ::read(fd, data + received_,
                                  sizeof(report_) - received_);
            if (n > 0) {
                received_ += static_cast<std::size_t>(n);
            } else if (n == 0) {
                if (received_ != 0) {
                    throw std::runtime_error("启动错误报告不完整");
                }
                done_ = true;
                std::cout << "启动错误通道已关闭，未收到失败报告"
                          << std::endl;
                return;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            } else {
                throw std::system_error(errno, std::generic_category(),
                                        "读取启动错误报告失败");
            }
        }
        if ((report_.stage != LaunchStage::group &&
             report_.stage != LaunchStage::signals &&
             report_.stage != LaunchStage::exec) || report_.code <= 0) {
            throw std::runtime_error("启动错误报告格式不正确");
        }
        failed_ = true;
        done_ = true;
        std::cerr << "子进程启动失败，阶段：" << stage_name(report_.stage)
                  << "，原因："
                  << std::system_error(report_.code,
                                       std::generic_category()).what()
                  << std::endl;
    }

    bool done() const { return done_; }
    bool failed() const { return failed_; }

private:
    LaunchError report_{};
    std::size_t received_ = 0;
    bool done_ = false;
    bool failed_ = false;
};

// 本程序是单线程，且只有这个对象负责回收这一个直接子进程。
class ChildProcess {
public:
    explicit ChildProcess(pid_t pid) : pid_(pid) {}
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // 仅用于父进程异常退出路径：尝试终止，再回收；不承诺硬期限。
    ~ChildProcess() {
        if (pid_ <= 0) return;
        if (::kill(pid_, SIGKILL) < 0 && errno != ESRCH) {
            const char message[] = "异常清理：发送 SIGKILL 失败，继续尝试回收\n";
            (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
        }
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    }

    std::optional<int> try_wait() {
        if (pid_ <= 0) {
            throw std::logic_error("子进程已回收，不能重复等待");
        }
        int status = 0;
        for (;;) {
            const pid_t result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                return status;
            }
            if (result == 0) return std::nullopt;
            const int code = errno;
            if (code == EINTR) continue;
            // 若已失去回收关系，不能在析构中拿这个旧 PID 发信号。
            if (code == ECHILD) pid_ = -1;
            throw std::system_error(code, std::generic_category(),
                                    "查询子进程退出状态失败");
        }
    }

    bool send(int signo) const {
        if (pid_ <= 0) {
            throw std::logic_error("不能向已回收的子进程发送信号");
        }
        if (::kill(pid_, signo) == 0) return true;
        if (errno == ESRCH) return false;
        throw std::system_error(errno, std::generic_category(),
                                "向子进程发送信号失败");
    }

private:
    pid_t pid_;
};

int describe_status(int status) {
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        std::cout << "子进程正常退出，退出码：" << code << std::endl;
        return code;
    }
    if (WIFSIGNALED(status)) {
        const int signo = WTERMSIG(status);
        std::cout << "子进程被信号终止，信号编号：" << signo << std::endl;
        return 128 + signo;
    }
    throw std::runtime_error("收到非预期的子进程状态");
}

// 一轮最多读一个通知，持续到来的停止请求不能占住整轮循环。
std::optional<int> read_stop(int fd) {
    signalfd_siginfo info{};
    const auto n = ::read(fd, &info, sizeof(info));
    if (n == static_cast<ssize_t>(sizeof(info))) {
        return static_cast<int>(info.ssi_signo);
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                  errno == EINTR)) {
        return std::nullopt;
    }
    if (n < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "读取停止通知失败");
    }
    throw std::runtime_error("停止通知长度不正确");
}

enum class StopState { running, waiting, killing };

int wait_budget(StopState state, Clock::time_point deadline) {
    if (state != StopState::waiting) return kTickMs;
    const auto left = deadline - Clock::now();
    if (left <= Clock::duration::zero()) return 0;
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
    return static_cast<int>(std::min<long long>(
        kTickMs, std::max<long long>(1, ms)));
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "用法：process_supervisor 程序路径 [程序参数...]\n";
            return 125;
        }

        sigset_t stops;
        check_call(::sigemptyset(&stops), "初始化停止信号集合失败");
        check_call(::sigaddset(&stops, SIGINT), "添加 SIGINT 失败");
        check_call(::sigaddset(&stops, SIGTERM), "添加 SIGTERM 失败");
        // 父进程通过 signalfd 同步读取停止信号。
        check_call(::sigprocmask(SIG_BLOCK, &stops, nullptr),
                   "屏蔽停止信号失败");
        struct sigaction defaults{};
        defaults.sa_handler = SIG_DFL;
        check_call(::sigemptyset(&defaults.sa_mask), "初始化信号动作失败");
        for (int signo : {SIGINT, SIGTERM, SIGCHLD}) {
            check_call(::sigaction(signo, &defaults, nullptr),
                       "恢复默认信号动作失败");
        }
        const int signal_raw =
            ::signalfd(-1, &stops, SFD_NONBLOCK | SFD_CLOEXEC);
        check_call(signal_raw, "创建停止通知描述符失败");
        net::SocketFd signal_fd(signal_raw);

        int pipe_fds[2];
        check_call(::pipe(pipe_fds), "创建启动错误管道失败");
        net::SocketFd read_end(pipe_fds[0]);
        std::optional<net::SocketFd> write_end(
            std::in_place, pipe_fds[1]);
        set_cloexec(read_end.get());
        set_cloexec(write_end->get());
        set_nonblocking(read_end.get());

        std::cout << std::flush;
        std::cerr << std::flush;
        const pid_t child_pid = ::fork();
        check_call(child_pid, "创建子进程失败");
        if (child_pid == 0) {
            ::close(read_end.get());
            ::close(signal_fd.get());
            const int error_fd = write_end->get();
            // 孩子离开父进程的前台进程组，再解除信号屏蔽。
            if (::setpgid(0, 0) < 0) {
                child_failed(error_fd, LaunchStage::group, errno);
            }
            if (::sigaction(SIGINT, &defaults, nullptr) < 0 ||
                ::sigaction(SIGTERM, &defaults, nullptr) < 0 ||
                ::sigprocmask(SIG_UNBLOCK, &stops, nullptr) < 0) {
                child_failed(error_fd, LaunchStage::signals, errno);
            }
            ::execv(argv[1], argv + 1);
            child_failed(error_fd, LaunchStage::exec, errno);
        }

        ChildProcess child(child_pid);
        write_end.reset();
        // 父子双方设置同一组，缩小依赖谁先得到调度的窗口。
        // EACCES：孩子已经 exec；ESRCH：孩子可能已经退出。
        if (::setpgid(child_pid, child_pid) < 0 &&
            errno != EACCES && errno != ESRCH) {
            throw std::system_error(errno, std::generic_category(),
                                    "父进程设置子进程组失败");
        }

        std::cout << "监管器 PID：" << ::getpid()
                  << "，子进程 PID：" << child_pid
                  << "，停止宽限期：" << kGrace.count() << " 毫秒"
                  << std::endl;
        LaunchReader launch;
        StopState state = StopState::running;
        Clock::time_point deadline{};

        for (;;) {
            // 从启动阶段起就使用非阻塞读取，不等完整报告才处理停止。
            launch.pump(read_end.get());

            const auto status = child.try_wait();
            if (status) {
                // 孩子可能在上一次读管道后才写入报告并退出。
                launch.pump(read_end.get());
                if (!launch.done()) {
                    throw std::runtime_error("子进程退出后启动通道仍未结束");
                }
                const int child_code = describe_status(*status);
                const int code = launch.failed() ? 125 : child_code;
                std::cout << "子进程已回收，监管器退出码：" << code
                          << std::endl;
                return code;
            }

            if (const auto signo = read_stop(signal_fd.get())) {
                if (state == StopState::running) {
                    deadline = Clock::now() + kGrace;
                    state = StopState::waiting;
                    std::cout << "监管器收到 "
                              << (*signo == SIGINT ? "SIGINT" : "SIGTERM")
                              << "，向子进程发送 SIGTERM，开始等待"
                              << std::endl;
                    if (!child.send(SIGTERM)) {
                        std::cout << "发送时子进程已不存在，继续查询回收状态"
                                  << std::endl;
                    }
                } else {
                    std::cout << "停止流程已开始，不重置截止时间"
                              << std::endl;
                }
            }

            if (state == StopState::waiting && Clock::now() >= deadline) {
                state = StopState::killing;
                std::cout << "停止宽限期已到，向子进程发送 SIGKILL"
                          << std::endl;
                if (!child.send(SIGKILL)) {
                    std::cout << "发送时子进程已不存在，继续查询回收状态"
                              << std::endl;
                }
            }

            pollfd watched[2]{
                {signal_fd.get(), POLLIN, 0},
                {launch.done() ? -1 : read_end.get(), POLLIN, 0}
            };
            const int ready =
                ::poll(watched, 2, wait_budget(state, deadline));
            if (ready < 0) {
                if (errno == EINTR) continue;
                check_call(ready, "等待监管事件失败");
            }
            for (const auto& item : watched) {
                if (item.revents & (POLLERR | POLLNVAL)) {
                    throw std::runtime_error("监管描述符状态异常");
                }
            }
            // 管道 POLLHUP 不是丢弃数据的理由；下轮 pump 读完再判 EOF。
            // 本例只有两项，醒来后统一尝试非阻塞读取即可。
        }
    } catch (const std::exception& error) {
        std::cerr << "监管器失败：" << error.what() << '\n';
        return 125;
    }
}
