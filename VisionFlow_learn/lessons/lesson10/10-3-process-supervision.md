# 10-3：从等待退出到进程监管

本课熟悉程度：你已明确表示“完全不熟悉”。模块 1～9、10-1 和 10-2 已完成，本课不重讲旧课。

本课要亲手完成一件事：父进程收到停止请求后，给子进程一次自行清理的机会；期限到了仍未结束，就请求内核强制终止；无论走哪条路，父进程都负责回收并报告退出状态。

老师维护本讲义、README、HANDOFF 和 CMake 配套。下面的两个主要入口由你新建，配置、构建、运行和观察也由你执行。讲义代码经过静态核对，尚未由老师编译或运行。

## 1. 从上节的具体限制出发

上节启动器的主线是：

~~~text
创建孩子 → 读取启动错误管道 → waitpid(..., 0) → 报告结果
~~~

最后的 waitpid 会等到孩子退出。如果孩子一直计算、忽略停止请求或者卡住，父进程就一直停在这里，不能主动执行“已经等了三秒，下一步怎么办”。

还有一个更早的阻塞点：读取启动错误管道。仅把最后的 waitpid 改成非阻塞不够；如果子进程在 exec 前迟迟没有进展，父进程可能连最后那句都到不了。

本课将它们放入同一个循环：

~~~text
读一点启动报告 → 查一次孩子是否退出 → 处理停止请求
       ↑                                  ↓
  等事件或短暂超时 ← 检查停止期限是否已到
~~~

循环只是协调这些事情，不替子进程完成图片计算，也不使用线程池。

## 2. 先分清“请求停止”“强制终止”和“回收”

### 2.1 SIGTERM：请求对方结束

父进程可以调用：

~~~cpp
::kill(child_pid, SIGTERM);
~~~

kill 是接口名字；它的功能是发送信号，并不总意味着立即杀死。

- child_pid 是我们 fork 出来的、尚未回收的直接子进程 PID，必须是正数。
- SIGTERM 表示请求终止；默认动作是终止，但程序也可以处理、屏蔽或者忽略它。
- 返回 0 表示此次发送成功，不代表对方已经退出，更不代表已经完成清理。
- 返回 -1 时看 errno。ESRCH 表示没有找到目标；其他错误如 EPERM 表示无权发送，不能假装成功。

本课仅向这个正 PID 发送，不使用 kill(0, ...)、kill(-1, ...) 或负的组 ID；它们的作用范围不同。[kill 接口手册](https://man7.org/linux/man-pages/man2/kill.2.html)

对于现有 vision_pool_server，SIGTERM 可以被它自己的 signalfd 接收，进入 10-1 的停止流程。因此“父进程请求停止”和“服务端自己清理”是两个进程之间的协作。

### 2.2 宽限期：给它多少清理时间

本课固定为 3000 毫秒，计时从父进程第一次读到停止请求时开始。

它不是服务端从启动起只能活三秒，也不是每执行一次 poll 都重新给三秒。

~~~text
收到首次停止请求时刻 = t0
截止时间 = t0 + 3 秒

之后的每一轮只比较“现在是否到达截止时间”。
重复 Ctrl+C 不延长这个截止时间。
~~~

用 steady_clock 计量时间间隔，避免把用户调整系统日期、墙上时间跳变等因素带入停止策略。

### 2.3 SIGKILL：程序不能自行拒绝

宽限期到达后，如果还没观察到孩子退出，就发 SIGKILL。SIGKILL 不能被捕获、屏蔽或忽略；目标程序没有机会为它执行 C++ 析构或自己的正常清理流程。[Linux 信号语义](https://man7.org/linux/man-pages/man7/signal.7.html)

这意味着尚未发完的响应、尚未写完的业务数据可能中断。三秒是本课的观察参数，真实部署应结合允许的任务时长决定。

**发出 SIGKILL 后仍然要回收。** 发信号是一次动作，取得退出状态并释放剩余进程记录是另一件事。

也不能把它理解为“最多三秒整个监管器一定退出”：调度延迟、不可中断的内核等待等会影响实际结束时刻。我们实现的是到期升级动作，不是系统层面的硬实时期限。

## 3. 本课的状态只有三种

| state | 含义 | 后续动作 |
| --- | --- | --- |
| running | 未收到停止请求 | 查退出状态，等停止通知 |
| waiting | 已发 SIGTERM，正在宽限期内 | 保留同一个 deadline，等孩子自行退出 |
| killing | 已到期并尝试发送 SIGKILL | 继续查退出状态，不重复发 SIGKILL |

任意状态下，一旦 waitpid 成功取得终止状态，就结束监管循环。

~~~mermaid
stateDiagram-v2
    [*] --> running
    running --> waiting: 首次停止请求 / SIGTERM
    waiting --> killing: 到达 deadline / SIGKILL
    running --> [*]: 回收成功
    waiting --> [*]: 回收成功
    killing --> [*]: 回收成功
~~~

重复停止请求在 waiting/killing 中只提示“已经开始”，不重置 deadline，也不定义“第二次 Ctrl+C 立即强杀”。

这里的状态描述停止过程；启动错误管道另有自己的读取进度，两个状态可以同时存在。例如：目标还未 exec，父进程已经收到停止请求。

## 4. 三个接口怎样组成循环

### 4.1 waitpid 加 WNOHANG：查一次就回来

~~~cpp
int status = 0;
pid_t result = ::waitpid(child_pid, &status, WNOHANG);
~~~

| 返回值 | 对本课的含义 | status 能否作为新退出结果使用 |
| --- | --- | --- |
| child_pid | 这个孩子已结束，本次成功回收 | 可以，先分类再取值 |
| 0 | 孩子存在，但此刻没有可回收的终止状态 | 不可以 |
| -1 | 调用失败，查看 errno | 不可以 |

WNOHANG 不结束孩子、不建立后台线程，也不替我们安排下次检查。下次什么时候查由事件循环决定。本课不使用 WUNTRACED 等选项，不在调试器跟踪状态下做演示。[waitpid 返回值与选项](https://man7.org/linux/man-pages/man2/waitpid.2.html)

代码把“还没结束”表达成 std::nullopt，把“已回收的编码 status”放进 optional<int>。正常退出 0 时，optional 仍然有值，if (status) 检查的是有没有结果，不是整数是否非零。

### 4.2 signalfd：父进程收自己的停止请求

沿用 10-1 的做法，父进程屏蔽 SIGINT、SIGTERM，再从非阻塞 signalfd 读取通知。屏蔽不等于忽略，也不是把父进程暂停。

这里选择单线程监管器，所以使用 sigprocmask，失败按 -1/errno 处理。没有异步信号处理函数，也不在信号处理函数中操作 cout、容器或进程对象。

父进程读取的是发给父进程的停止请求。给孩子发信号由另一句 kill 完成，两者不能混为一谈。[signalfd 用法与继承](https://man7.org/linux/man-pages/man2/signalfd.2.html)

### 4.3 poll：没事时先睡一小会儿

本课只有两个描述符：signalfd 和启动错误管道读端。使用 poll 同时等待它们，避免额外建立 epoll 注册表；这是接口选择，不改变原服务端的 epoll。

~~~cpp
pollfd watched[2]{
    {signal_fd.get(), POLLIN, 0},
    {launch.done() ? -1 : read_end.get(), POLLIN, 0}
};
int ready = ::poll(watched, 2, timeout_ms);
~~~

- fd：要观察哪个描述符；负值表示忽略这一项。
- events：想等什么，这里是可读 POLLIN。
- revents：内核返回的实际事件。
- 2：数组有两项。
- timeout_ms：这次等待的毫秒数。
- 返回正数：有事件的数组项数；0：超时；-1：出错，EINTR 时回到循环重查。

运行中和强杀后的等待参数为 100 毫秒。宽限期中则取“100 毫秒”和“剩余时间”的较小值；不足一毫秒的正剩余量按一毫秒等待，避免长时间忙转。超时与调度允许有误差，不保证精确唤醒。

本课没有把 SIGCHLD 加入 signalfd，定期查询足以回收一个孩子。因此，即使子进程退出没有让当前 poll 立即醒来，也会在后续循环中检查。

管道的 POLLHUP 可能和未读完的数据同时存在。必须读完后再判断 EOF；不能看见 HUP 就扔掉启动失败报告。[poll 参数与事件](https://man7.org/linux/man-pages/man2/poll.2.html)

## 5. 为什么引入一个独立的子进程组

上一课父子通常在同一个前台进程组，Ctrl+C 由终端同时发送给它们；那不叫父进程转发。

本课希望正常就绪后的路线变成：

~~~text
终端 Ctrl+C
    ↓ SIGINT
监管器的 signalfd
    ↓ kill(子 PID, SIGTERM)
子程序自己的停止逻辑
~~~

因此 fork 后把孩子放入以它自己的 PID 为组号的进程组：

~~~cpp
// 子分支
::setpgid(0, 0);

// 父分支
::setpgid(child_pid, child_pid);
~~~

子分支的两个 0 分别表示当前进程和以当前 PID 作为 PGID。父子两边都设置同一个组，减少“谁先运行”的影响。父进程若遇到 EACCES，可能是孩子已经完成 exec；孩子在 exec 前已经执行过自己的设置。ESRCH 则交给退出查询处理，其他错误明确报告。[进程组接口](https://man7.org/linux/man-pages/man2/setpgid.2.html)

注意这些边界：

1. 孩子仍然是这个父进程的直接子进程，父进程仍能 waitpid 回收它。改变进程组不等于改变父子关系。
2. 本课 kill 的参数仍是正的 child_pid，只监管这一个进程，不监管它未来 fork 出的整棵后代树。
3. 这不是完整守护进程化，也不改变当前目录。孩子继承标准输出，通常仍能在原终端打印日志。
4. 孩子处于后台进程组，不适合作为需要从终端读取交互输入的程序。本课样例和服务端均不读终端输入；终端使用通常的允许后台输出设置。
5. fork 到设置组之间仍有短窗口，极早的 Ctrl+C 可能同时到达孩子；孩子解除屏蔽后可能按默认动作结束。父进程仍会回收它，但不能把这种情况当成“已就绪后的停止观察”。实践要等子程序明确打印就绪，再按 Ctrl+C。

## 6. 本次由你创建的文件

学习工程根目录：`/home/rikanshin/VisionFlow/VisionFlow_learn`。

| 文件 | 操作与职责 |
| --- | --- |
| `/home/rikanshin/VisionFlow/VisionFlow_learn/apps/process_supervisor.cpp` | 新建，放入第 7 节完整监管器代码 |
| `/home/rikanshin/VisionFlow/VisionFlow_learn/apps/shutdown_child.cpp` | 新建，放入第 8 节完整观察子程序 |
| `/home/rikanshin/VisionFlow/VisionFlow_learn/CMakeLists.txt` | 老师已在末尾增加条件式目标，不需要再粘贴一次 |
| `/home/rikanshin/VisionFlow/VisionFlow_learn/apps/process_launcher.cpp` | 保留上一课实现和你的注释 |
| `/home/rikanshin/VisionFlow/VisionFlow_learn/apps/process_child.cpp` | 保留上一课退出方式示例 |
| `/home/rikanshin/VisionFlow/VisionFlow_learn/apps/vision_pool_server.cpp` | 直接作为监管器启动的目标，沿用已有实现 |

本课仍复用 tcp_io.hpp / tcp_io.cpp 中的 SocketFd 管理普通文件描述符；它的析构调用 close，也能关闭管道和 signalfd。名字含 Socket 不意味着内核的 close 只能处理 socket。

不把新入口粘进旧入口的 main；这两个新文件分别各自拥有一个 main。

## 7. 新建 process_supervisor.cpp：完整代码

先按下面完整内容建立入口，然后对照第 9～12 节理解对象和每轮状态的变化。这里展示完整文件，不要求你猜省略部分。

~~~cpp
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
~~~

## 8. 新建 shutdown_child.cpp：完整代码

这个小程序让我们可控地观察两条停止路径，而不需要把图像算法故意写死：

- --cooperate：等 SIGTERM，模拟清理一秒，再 return 0。
- --ignore-term：明确忽略 SIGTERM，一直等待；由监管器在期限后终止它。

必须等“观察子进程已就绪”出现再发送停止请求。目标刚进入 main、尚未安装上述行为时，SIGTERM 仍可能按默认动作直接结束它。

~~~cpp
#include <cerrno>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <time.h>
#include <unistd.h>

namespace {
void check_call(int result, const char* message) {
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), message);
    }
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            throw std::runtime_error(
                "用法：shutdown_child --cooperate 或 --ignore-term");
        }
        const std::string mode = argv[1];
        if (mode != "--cooperate" && mode != "--ignore-term") {
            throw std::runtime_error("未知观察模式");
        }

        sigset_t term;
        check_call(::sigemptyset(&term), "初始化信号集合失败");
        check_call(::sigaddset(&term, SIGTERM), "添加 SIGTERM 失败");

        if (mode == "--ignore-term") {
            struct sigaction ignored{};
            ignored.sa_handler = SIG_IGN;
            check_call(::sigemptyset(&ignored.sa_mask), "初始化信号动作失败");
            check_call(::sigaction(SIGTERM, &ignored, nullptr),
                       "忽略 SIGTERM 失败");
            check_call(::sigprocmask(SIG_UNBLOCK, &term, nullptr),
                       "解除 SIGTERM 屏蔽失败");
            std::cout << "观察子进程已就绪：PID=" << ::getpid()
                      << "，PGID=" << ::getpgrp()
                      << "，模式=忽略 SIGTERM" << std::endl;
            for (;;) ::pause();
        }

        check_call(::sigprocmask(SIG_BLOCK, &term, nullptr),
                   "屏蔽 SIGTERM 失败");
        std::cout << "观察子进程已就绪：PID=" << ::getpid()
                  << "，PGID=" << ::getpgrp()
                  << "，模式=配合停止" << std::endl;

        for (;;) {
            const int signo = ::sigwaitinfo(&term, nullptr);
            if (signo == SIGTERM) break;
            if (signo < 0 && errno == EINTR) continue;
            check_call(signo, "等待 SIGTERM 失败");
        }
        std::cout << "收到 SIGTERM，模拟清理 1000 毫秒" << std::endl;
        timespec remaining{1, 0};
        for (;;) {
            timespec next{};
            if (::nanosleep(&remaining, &next) == 0) break;
            if (errno == EINTR) {
                remaining = next;
                continue;
            }
            check_call(-1, "模拟清理等待失败");
        }
        std::cout << "清理完成，正常退出" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "观察子进程失败：" << error.what() << '\n';
        return 1;
    }
}
~~~

样例先屏蔽 SIGTERM，再通过 sigwaitinfo(&term, nullptr) 同步等待并取出这个信号。第二个参数是可选的信号详情输出，本例不需要所以传 nullptr。成功返回信号编号，失败返回 -1 并设置 errno。

这里允许阻塞：样例子进程只需要等待一个停止请求；父进程要同时关注期限，所以不能套用这种无限等待。[sigwaitinfo 手册](https://man7.org/linux/man-pages/man2/sigwaitinfo.2.html)

timespec remaining{1, 0} 表示一秒零纳秒。nanosleep 的第二个参数接收被中断后的剩余时间；遇到 EINTR 后等剩余部分，而不是重新等一秒。这只模拟清理，不是实际图片处理的耗时。

忽略模式中的 pause() 使程序睡眠等待信号，不是空循环消耗 CPU。被忽略的 SIGTERM 不会触发清理输出，也不会让它正常退出。SIGKILL 的作用由内核完成。

## 9. 从 main 看父子双方各自做什么

### 9.1 fork 之前：创建停止通知和错误通道

顺序不能随意颠倒：

~~~text
准备 SIGINT/SIGTERM 集合
→ 父进程屏蔽这两个信号
→ 将 SIGINT/SIGTERM/SIGCHLD 动作明确设为默认
→ 创建非阻塞、exec 时关闭的 signalfd
→ 创建错误管道并包装进 RAII 对象
→ 写端设置 FD_CLOEXEC，读端设置 FD_CLOEXEC 和 O_NONBLOCK
→ 刷新已有输出缓冲
→ fork
~~~

父进程不再像旧启动器那样忽略 SIGINT。它保留信号的产生，通过 signalfd 消费通知。SIGCHLD 设为默认，为本课普通 waitpid 回收方式保留退出状态。

两个 fcntl 辅助函数不要混用：

| 操作 | 管什么 | 本课用途 |
| --- | --- | --- |
| F_GETFD / F_SETFD | 描述符标志 | 加上 FD_CLOEXEC |
| F_GETFL / F_SETFL | 文件状态标志 | 给管道读端加 O_NONBLOCK |

设置时与已有 flags 按位或，保留原来的其他标志。

错误管道的写端保持阻塞。它只由启动失败的子分支写一份很小的报告，不承担持续业务流量；父进程循环读取。读端非阻塞使父进程能在报告尚未到达时继续处理停止请求。

### 9.2 fork 后的子分支

子分支先关闭它不需要的管道读端和父进程的 signalfd 副本，再设置自己的进程组、恢复信号动作并解除停止信号屏蔽，最后 exec。

为什么不把父进程 signalfd 留给孩子使用？因为它是父进程的监管入口。孩子进入目标程序后应建立自己的信号策略；现有服务端会自行创建自己的 signalfd。

任何准备阶段失败，都通过 child_failed 写 LaunchStage 和 errno，然后 _exit(127)。本课新增 group 阶段，便于区分进程组设置失败、信号设置失败和 exec 失败。

本课的 LaunchStage 数值与旧启动器不完全相同，但错误管道只在同一个新程序 fork 出来的父子间使用，不与旧启动器互通，也不是网络 VFLW 协议。

### 9.3 fork 后的父分支

父进程立刻建立 ChildProcess 对象，承担孩子的回收责任，然后关闭自己的错误管道写端。

之后设置孩子进程组、打印两个 PID，进入监管循环。不能拿日志里的“监管器 PID”当作孩子 PID；两个数分别有不同用途。

本课是一个独立可执行程序，不把修改过的信号集合交回长期运行的宿主程序。因此没有在返回 main 前恢复旧屏蔽集。若将来改成可复用库接口，还要设计信号策略的保存与恢复。

## 10. 非阻塞错误管道：保存什么，什么时候继续

LaunchReader 对象保存四项：

| 字段 | 用途 |
| --- | --- |
| report_ | 接收那一份 LaunchError 的内存 |
| received_ | 已经读入多少字节 |
| done_ | 读完合法报告，或者无报告 EOF |
| failed_ | 是否确实读到了合法启动失败报告 |

它不拥有 fd。read_end 对象负责关闭读端，LaunchReader 只保存读入的数据与进度。

一次 pump 的行为：

~~~text
读到正数字节 → received_ 累加，从新偏移继续读
EAGAIN      → 保留 report_ / received_，返回监管循环
EINTR       → 重试当前读取
EOF 且 0 字节 → 完成，但没有失败报告
EOF 且部分报告 → 报告不完整，明确出错
读满         → 校验 stage/code，记录启动失败
~~~

这与模块 8 保存头部进度的思想相同，只是数据从管道而不是 TCP 来。这里不重新学习分段读取；关注的是它如何让“启动过程中也能停止”成立。

为什么 done 后把 poll 的第二项设成 -1？已经到 EOF 的管道会持续呈现可读或挂断状态。如果继续监视，poll 可能每次立即返回，让监管器无意义地空转。

为什么在 waitpid 成功之后又 pump 一次？可能发生这样的交错：

~~~text
父：读管道，此刻还没有报告，返回 EAGAIN
子：写入报告 → _exit(127)
父：waitpid 成功回收
父：再读取管道，拿到刚写入的报告
~~~

管道中的数据不会因为写入者已经被回收而自动消失。最后这次读取避免把启动失败误当成普通退出 127。

无报告 EOF 仍不等于目标业务就绪。服务端是否已监听仍看它自己的监听日志，样例是否已安装停止策略仍看“观察子进程已就绪”。

## 11. ChildProcess：查询、发送与责任结束

### 11.1 try_wait 不会无限等待

它只使用正 PID 加 WNOHANG。返回有值时，将 pid_ 置为 -1，明确标记已经回收。之后 send 和 try_wait 都拒绝再次使用这个失效状态。

EINTR 重试，其他错误抛出。若是 ECHILD，代码先使 pid_ 失效，再抛出错误，避免后续异常清理仍向一个失去回收关系的旧数字发信号。

本课 SIGCHLD 不显式忽略、没有其他线程或其他 wait 调用抢先回收。只要回收责任仍属于这里，退出但未回收的孩子仍保留进程记录，PID 不会被当成另一进程的新 PID 使用。这个结论依赖本课的所有权约束，不是“所有保存下来的 PID 永远安全”。

### 11.2 send 不是 wait

send(SIGTERM) 或 send(SIGKILL)：

- 发送成功返回 true。
- ESRCH 返回 false，让循环继续查询退出状态。
- 其他错误抛异常。
- 不减少某个“任务数”，也不把 pid_ 提前置为 -1。

即使内核接受 SIGKILL，也要由 try_wait 成功后才确认回收完成。若子进程恰好在检查后自行退出，发送结果和最终退出方式也可能与想象不同；最终使用 waitpid 的状态，而不是仅凭“曾尝试发过 SIGKILL”就硬写 137。

### 11.3 析构只负责异常路径兜底

正常路径已经显式回收，pid_ 为 -1，析构不会再发信号。

父进程中途遇到异常时，本课采用一个明确策略：尝试 SIGKILL，再阻塞 waitpid 回收。它不再执行三秒优雅等待，因为监管循环已经失败。

这也意味着：

- 普通正常停止的宽限期由主循环负责。
- 异常清理可能等待，发送失败也会尝试回收，不承诺有限时间。
- 析构不抛异常；它只是兜底尝试，不证明所有系统错误都能恢复。
- 父进程自己被 SIGKILL 时不会执行 C++ 析构，本课不保证此时孩子自动退出。

不要把异常兜底路径当成“正常状态机已经验证”；本课不要求你故意制造权限变化或内核故障。

## 12. 按一轮循环推演

### 12.1 为什么先查退出，再处理停止

本课优先取得已经可回收的结果。若孩子已经正常退出，父进程就报告真实结果并返回；没有必要再发送停止请求。

每轮顺序是：

1. pump 启动报告。
2. try_wait 查询退出；成功则补读报告、分类退出状态并返回。
3. 从 signalfd 最多取一个停止通知。
4. 首次停止：记录一次 deadline，状态改为 waiting，发送 SIGTERM。
5. waiting 且到期：状态改为 killing，发送一次 SIGKILL。
6. poll 等待事件或本轮超时，然后开始下一轮。

一次最多取一个停止通知，避免不停读通知占住整轮，导致期限检查迟迟没有机会执行。普通信号可能合并，不应把“收到多少条日志”当成按键次数的精确计数。

### 12.2 配合停止的时间线

~~~text
t0         监管器收到 SIGINT，deadline=t0+3s，发送 SIGTERM
t0 附近    子程序收到 SIGTERM，开始清理
t0+约1s    子程序 return 0
随后一轮   waitpid 回收，打印正常退出 0，监管器 return 0
~~~

父进程没有为此 sleep(3)。它边等待边查退出，所以孩子提前完成就提前结束。

### 12.3 忽略停止的时间线

~~~text
t0         收到请求，发送 SIGTERM，开始三秒宽限期
t0～t0+3s  子程序忽略 SIGTERM；父进程持续协调事件与查询
到期后     父进程发送 SIGKILL，状态变 killing
之后       waitpid 确认被信号 9 终止，监管器映射为 128+9=137
~~~

计时从日志对应的首次请求处理开始；不是从程序启动开始。实际输出间隔受调度和日志 I/O 影响，不把示意时间线当性能测量结果。

### 12.4 退出码策略

| 最终情况 | 监管器返回值 |
| --- | --- |
| 子进程正常退出，无启动失败报告 | 原正常退出码 |
| 子进程被信号终止，无启动失败报告 | 128 + 信号编号 |
| 收到启动失败报告 | 125 |
| 监管器自己的参数或运行错误 | 125 |

即使监管器曾收到 Ctrl+C，只要子程序成功清理并正常退出 0，本课监管器也返回 0。没有规定“按过 Ctrl+C 就一定返回 130”。

137 是这里常见的 SIGKILL 结果；它不是 SIGKILL 编号，也不是被强杀程序正常执行了 return 137。单凭 shell 一个数字也不能区分目标主动 return 137，仍要结合退出方式日志。

## 13. CMake 配套与由你执行的实践

### 13.1 CMake 已由老师维护

老师在 `/home/rikanshin/VisionFlow/VisionFlow_learn/CMakeLists.txt` 末尾追加以下目标。仅供理解，不重复追加：

~~~cmake
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/shutdown_child.cpp")
    add_executable(shutdown_child apps/shutdown_child.cpp)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/process_supervisor.cpp")
    add_executable(process_supervisor
        apps/process_supervisor.cpp
        src/tcp_io.cpp
    )
    target_include_directories(process_supervisor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
endif()
~~~

这两个入口不调用 OpenCV，也不创建 C++ 工作线程，所以目标本身不增加这些链接依赖。学习工程其他既有目标的依赖保持原配置。

写齐两个文件之后，你再执行：

~~~bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake -S . -B build
cmake --build build --target process_supervisor shutdown_child -j2
~~~

不要在文件还不存在时就以“目标没有生成”判断代码错误：条件式 CMake 需要你写齐之后重新配置。

### 13.2 观察一：给它机会，它自己退出

终端 A：

~~~bash
./build/process_supervisor ./build/shutdown_child --cooperate
~~~

等待“观察子进程已就绪……模式=配合停止”，再在同一终端按一次 Ctrl+C。

重点观察这条因果链：

~~~text
监管器收到 SIGINT，向子进程发送 SIGTERM，开始等待
收到 SIGTERM，模拟清理 1000 毫秒
清理完成，正常退出
子进程正常退出，退出码：0
子进程已回收，监管器退出码：0
~~~

回到 shell 后立即执行：

~~~bash
echo $?
~~~

预期为 0，且不应出现“停止宽限期已到”。

如果过早按 Ctrl+C，出现被信号 2 或 15 直接终止，就没有观察到“已就绪后配合清理”这条路径；按上面的就绪提示重新观察即可，不必修改算法。

### 13.3 观察二：拒绝配合，期限后强制终止

终端 A：

~~~bash
./build/process_supervisor ./build/shutdown_child --ignore-term
~~~

等“观察子进程已就绪……模式=忽略 SIGTERM”，再按一次 Ctrl+C。

预期先显示监管器发送 SIGTERM，但孩子没有“模拟清理”的输出。大约三秒后：

~~~text
停止宽限期已到，向子进程发送 SIGKILL
子进程被信号终止，信号编号：9
子进程已回收，监管器退出码：137
~~~

回到 shell 后：

~~~bash
echo $?
~~~

预期为 137。无需自己另开命令强杀，也不需要反复按 Ctrl+C。

### 13.4 观察三：监管已有服务端

先确认之前的服务端已停止，避免 19090 端口被占用。终端 A：

~~~bash
./build/process_supervisor ./build/vision_pool_server
~~~

等服务端显示监听，再在终端 B 运行现有客户端：

~~~bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
python3 python/vision_client.py samples/frame.jpg
echo $?
~~~

预期仍得到 request_id=42 的原样本成功结果。请求完成后，在终端 A 按一次 Ctrl+C。

这次关键日志应体现：

~~~text
监管器收到 SIGINT → 向孩子发送 SIGTERM
服务端收到 SIGTERM → 执行已有的优雅停止
服务端正常退出 → 监管器回收并报告退出码 0
~~~

服务端此时应该读到 SIGTERM；因为监管器将两种外部停止请求统一转成 SIGTERM。对比旧启动器中由终端直接发给服务端的 SIGINT，才能看到这次“主动发送”带来的实际变化。

终端 A 回到提示符后执行 echo $?，预期 0。本课不重复六请求或带任务排空实验，也不修改既有网络客户端。

注意：这里不加 --demo-delay-ms 5000。三秒宽限期可以在五秒模拟计算结束前触发强杀，那属于当前策略的结果，不能期待它同时保证五秒任务完整交付。

### 13.5 可选观察，不作为必须补交项

下面几项只在你想进一步观察时做，老师不要求全部补跑：

- 用新监管器启动旧 process_child --exit-code 7：未收到停止请求也能自动查询到退出并传回 7。
- 用新监管器启动不存在的路径：非阻塞错误管道报告失败，最终仍返回 125。
- 忽略模式中，在首次 Ctrl+C 之后约一秒再按一次：截止时间不重新起算。
- 在另一个终端执行 `kill -TERM 实际监管器PID`：仅向本次日志中的监管器正 PID 发送，观察它读取 SIGTERM 后再向孩子发送 SIGTERM。不要使用示例数字或旧 PID，也不要向进程组广播。

本课完成反馈只需构建结果、三项主要观察的关键日志和紧接着的退出码。老师没有替你运行这些命令；预期值不是已经取得的测试结果。

## 14. 常见问题从哪里定位

| 现象 | 先看哪里 |
| --- | --- |
| 新目标不存在 | 两个文件是否写齐，之后有没有重新执行 CMake 配置 |
| 子程序一启动就终止，没有清理日志 | 是否在它打印“已就绪”前发了信号 |
| 父进程卡在读取启动报告 | 读端是否设置 O_NONBLOCK，父进程是否关闭自己的写端 |
| CPU 持续空转 | launch.done 后是否停止监视 EOF 管道；是否有 poll 等待 |
| 配合模式等满三秒才结束 | 是否仍调用旧的阻塞等待／sleep，而没有每轮 try_wait |
| 连续 Ctrl+C 导致不断续命 | 是否每次通知都重新设置了 deadline |
| 已发送 SIGKILL 却没退出日志 | 发信号不等于已回收；查看实际 waitpid 状态，不伪造 137 |
| 服务端日志收到 SIGINT 而非预期 SIGTERM | 是否启动了新监管器，是否等到就绪，是否正确建立子进程组 |
| 目标读终端时停住 | 本课目标处于后台组，不支持这种交互输入；使用本课给定目标 |
| 服务端因端口占用立即退出 | 先停止之前那一个实例，不要改新监管器掩盖启动错误 |

## 15. 集中理解检查

先完成本课主要实践，再一次回答下面五点，不需要每读几行就暂停答题：

1. kill(child_pid, SIGTERM) 返回 0，能不能说明孩子已经清理完并退出？SIGTERM、SIGKILL、waitpid 分别承担什么角色？
2. waitpid(..., WNOHANG) 返回 0 时，该不该把 status 当作退出码？为什么不用 while(true) 不停查询，而要在循环里 poll？
3. 假设 t=10 秒首次读到停止请求，t=11 秒又收到一次，三秒宽限期的截止时间应是多少？为什么用 steady_clock？发送 SIGKILL 后能不能立即宣称已经回收？
4. 为什么错误管道也必须非阻塞并保留 received_？为什么 waitpid 回收成功后还要补读一次启动报告？
5. 子进程组分开后，就绪状态下按 Ctrl+C 的完整信号路线是什么？SIGKILL 导致退出是否会执行子程序自己的清理？本课能否保证整个后代进程树被停止、或者严格三秒内所有事情结束？

## 15.1 本次五点解答（2026-10-13）

学生要求“你帮我回答，然后直接开启下一环节”，因此由老师直接解释，不再要求学生重答或追加确认。

1. kill(child_pid, SIGTERM) 返回 0 只说明发送成功，不证明清理或退出已完成。SIGTERM 是可由子程序处理、屏蔽或忽略的停止请求；SIGKILL 请求内核强制终止，不能被目标捕获、屏蔽或忽略；waitpid 取得终止状态并完成回收。子进程已退出与父进程已回收是两件事。
2. WNOHANG 返回 0 表示本次尚无可回收的终止状态，不能据此使用 status 作为新的退出结果。只有返回对应子 PID 时才分类解读 status。本课通过 poll 等描述符事件或短暂超时，避免持续查询消耗 CPU，同时让父进程有机会检查停止请求和期限。
3. 第 10 秒首次处理停止请求，三秒宽限期的 deadline 固定为第 13 秒；第 11 秒重复请求不续期。steady_clock 用于单调时间间隔，不受墙上时间调整影响。到期发送 SIGKILL 是一次动作，之后仍需 waitpid 成功才算回收；实际退出不承诺精确发生在第 13 秒。
4. 如果错误管道仍阻塞，子分支在 exec 前没有进展时，父进程可能卡在读取中，无法处理停止和期限。received_ 保留已读的部分报告，下次从同一偏移继续。第一次 pump 返回 EAGAIN 之后，孩子可能才写入报告并退出；父进程 waitpid 成功后再 pump，才能读取这份已留在管道中的报告，避免把 exec 失败误判为普通退出 127。回收写入者不等于清空管道数据。
5. 子进程组已分开且目标已就绪时：终端向前台组发 SIGINT，监管器从自己的 signalfd 读取，再向直接子 PID 发送 SIGTERM；目标按自己的策略处理。SIGKILL 终止不运行目标的 C++ 析构或业务清理，但内核仍处理进程资源，父进程仍须回收。本课只给正的直接子 PID 发信号，不管理其整个后代树；调度、不可中断内核等待等也使严格三秒整体退出无法保证。

结合此前学生实现和三项主要观察，按本次明确要求收尾 10-3 与模块 10，直接进入模块 11。不记录为学生独立答对这五题，也不凭此宣称所有异常分支或硬实时能力已经验证。

## 16. 当前状态和接续边界

2026-10-13（10-3 与模块 10 按学生要求收尾）：学生明确要求“你帮我回答，然后直接开启下一环节”。老师直接解释了信号发送与退出／回收的区别、WNOHANG 返回 0 与 poll 等待、首次停止固定截止时间和单调时钟、非阻塞错误管道保存进度及回收后补读、进程组分离后的信号路线与强杀边界。完整解答已补入 10-3 讲义第 15.1 节。结合此前两份实现和三项主要运行观察，按本次明确要求收尾模块 10；不记录为学生独立答对五题，也不另要求重答或确认。此前未展示的 echo $?、可选与异常分支仍不倒填。随后进入 11-1，学生明确表示完全不熟悉，需要从头讲。老师未修改主要源码或配置、构建、运行、测试。

2026-10-13（10-3 实现与三项主要运行观察通过，集中理解检查待完成）：学生已新建 apps/process_supervisor.cpp 和 apps/shutdown_child.cpp；老师只读核对两份完整源码与当前讲义一致，没有修改源码。配合停止：监管器 PID 4069、子 PID/PGID 4070；就绪后 Ctrl+C，监管器收到 SIGINT 并发送 SIGTERM，子程序打印模拟清理 1000 毫秒、清理完成、正常退出，随后被回收，监管器日志退出码 0，没有宽限期到期日志。忽略模式：监管器 PID 4209、子 PID/PGID 4210；就绪后 Ctrl+C，先发送 SIGTERM，再出现宽限期到期及发送 SIGKILL 日志，子程序被信号 9 终止并回收，监管器日志退出码 137。日志没有时间戳，不据此宣称实测恰好三秒。真实服务端：监管器 PID 4384、子 PID 4385 与服务端 PID 一致，普通请求 42 返回既有 lane_lines 结果（640×360、3 通道、2095 个边缘像素、三条既有线段）；请求完成后 Ctrl+C，监管器收到 SIGINT、向孩子发送 SIGTERM，服务端明确收到 SIGTERM，此时未交回任务 0、连接数 0；完成停止、工作线程回收后正常退出，监管器回收并在日志中报告退出码 0。本轮未展示配置／构建输出、shell 的 echo $? 或客户端退出码，不倒填，不要求为此重跑。三项主要观察已齐，下一步只做本课五道集中理解检查；可选的普通退出、缺失路径、重复停止、向父 PID 单独发 SIGTERM 及异常分支未提供运行证据，不强制补跑。10-3 和模块 10 尚未全部完成，不提前进入模块 11。老师仅只读核对并维护教学资料，没有配置、构建、运行或测试。

以下为备课时的历史状态，以以上最新记录为准：

2026-10-12：10-3 熟悉程度已确认为“完全不熟悉”，不再询问。完整讲义和条件式 CMake 配套已准备。两个新入口由学生亲手新建，实现、构建、运行反馈和集中理解检查尚未完成。老师只静态核对资料和代码示例，没有写入两个主要源码文件，没有配置、构建、运行或测试。

本课只监管一个直接子进程；不自动重启、不做后代进程树管理、不实现业务就绪握手或硬实时退出保证。模块 10 已按学生明确要求收尾，接续模块 11，不把收尾理解为上述扩展能力已经实现。

资料中的 Linux 接口语义已对照链接的 Linux man-pages；课程的状态机、参数和观察安排是本项目的教学设计。
