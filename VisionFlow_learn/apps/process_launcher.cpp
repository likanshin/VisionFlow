// 模块 10.2 的启动器：fork + execv + waitpid 的最小骨架。
// 你每天都在用它——shell 执行一条命令，做的就是这三件事。
//
// 它还演示了一件容易被忽略的事：光看子进程的退出码，
// 区分不了“exec 失败”和“程序自己返回了 127”，
// 所以要另外开一条专用的启动错误管道把这个信息传回来。
#include "tcp_io.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace net = visionflow::net;

namespace {

// 这个枚举会跨进程边界经管道传输，所以显式指定底层类型为 int：
// 两端要按同一份内存布局解读它，不能让编译器各自挑一个“合适”的类型。
//
// 从 1 开始而不是 0：下面那份零初始化的报告，stage 正好是 0，
// 于是“读到全零或残缺的数据”会被字段校验直接拦下。
enum class LaunchStage : int { signals = 1, exec = 2 };

// 直接按内存字节传输的小结构体。之所以能这么干，是因为两端来自同一次 fork、
// 同一个程序映像，ABI 完全相同。
// 它不是跨机器协议：没有 VFLW 头，也不适合推广成通用序列化方案——
// 网络课的协议要自己约定字段布局和字节序，因为对面是别的机器；
// 而这里的“对面”就是你自己的副本。
struct LaunchError {
    LaunchStage stage;
    int code;   // 失败那次调用的 errno
};

// 返回 -1 并把错误码写进 errno——syscall 那一族的约定。
// 本文件里所有系统调用都走它，包括 sigprocmask。
// 对照上一课：pthread_sigmask 是唯一的例外（返回错误码、不设 errno），
// 所以那边只能手写判断。
void check_call(int value, const char* message) {
    if (value < 0) {
        throw std::system_error(errno, std::generic_category(), message);
    }
}

// 这个类管的是“回收责任”，不是“子进程本身”：
// 它不持有 fd，也不能杀子进程、不能发信号。
// 名字容易让人以为它是个完整的进程句柄，其实它只做一件事——
// 保证这份 PID 迟早会被 waitpid 收掉。
class ChildProcess {
public:
    explicit ChildProcess(pid_t pid) : pid_(pid) {}
    // 回收责任只能有一份。如果能拷贝，两份对象都以为自己该 waitpid：
    // 一份先收掉，另一份就会去等别的孩子（waitpid(-1)）或拿到 ECHILD。
    // 这和 SocketFd 删除拷贝是同一个理由——所有权语义。
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // 兜底路径，不是主路径。
    // main 在显式 wait() 之前还有会抛异常的操作，一旦那里飞出去，
    // 没有这个析构，孩子就变成僵尸了。
    //
    // 能力边界（讲义 9.1）：可能阻塞、不强制终止子进程、不承诺截止时间；
    // waitpid 遇到非 EINTR 错误时也不会无限重试——析构里抛异常会直接 terminate。
    // 所以它是“兜底”，不是“监管器”。
    ~ChildProcess() {
        if (pid_ > 0) {   // 已经显式回收过（pid_ 被置 -1）就不重复等
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
                // 空循环体：只关心“直到不再是 EINTR 为止”。
                // 这里的 status 不往外传，主路径的等待由 wait() 负责。
            }
        }
    }

    // 返回的是 waitpid 的 status（内核编码），不是退出码。
    // 把它解释成人能读的结论，是 describe_status 的活儿。
    int wait() {
        // 防止把 -1 交给 waitpid：那会变成“等任意一个孩子”，
        // 状态还会张冠李戴。宁可在这里明确报错。
        if (pid_ <= 0) {
            throw std::logic_error("子进程已回收，不能重复等待");
        }
        int status = 0;
        for (;;) {
            const pid_t result = ::waitpid(pid_, &status, 0);
            if (result == pid_) {
                pid_ = -1;   // 标记“回收责任已完成”
                return status;
            }
            if (result < 0 && errno == EINTR) {
                // 被信号打断就重来。这里不用像 read/write 那样累计偏移——
                // waitpid 不产出数据，没有“已经收了一半”的进度要保留。
                continue;
            }
            throw std::system_error(errno, std::generic_category(),
                                    "等待子进程失败");
        }
    }

private:
    pid_t pid_;
};

// 运行时机很特殊：fork 之后、exec 之前。这段代码跑在一个“危险窗口”里，
// 所以规矩和平时的 C++ 完全不同。
//
// [[noreturn]] 是给编译器的控制流承诺（本函数不会正常返回），
// 真正结束进程的是最后那行 _exit。
[[noreturn]] void child_failed(
    int error_fd, LaunchStage stage, int error_code) {
    const LaunchError report{stage, error_code};
    const auto* bytes = reinterpret_cast<const char*>(&report);
    // error_code 是调用方在进来之前就抓好的 errno 快照。
    // 不能在这里现读 errno：下面的 write 循环每次系统调用都会改写它，
    // 真正的原因（execv 到底为什么失败）会被 write 的 errno 覆盖掉。
    //
    // 循环发送：管道是字节流，一次 write 可能只写进去一部分（短写），
    // 所以要按实际写入量累计偏移，不假定一次就能送完一个结构体。
    std::size_t sent = 0;
    while (sent < sizeof(report)) {
        const auto count = ::write(error_fd, bytes + sent, sizeof(report) - sent);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            // 报告送不出去就放弃，绝不能因为报告失败把子进程卡在这里。
            // 父进程还有 waitpid 的状态和子进程退出码作为兜底信息。
            break;
        }
    }
    // 只能用 _exit，不能用 return，也不能用 exit()：
    //   return → 会回到父进程 main 的后半段继续跑（那时 child_pid 是 0），逻辑全乱；
    //   exit() → 会刷 stdio 缓冲、跑 atexit，把 fork 前缓冲的内容重复输出一遍，
    //            还会执行继承来的、本属于父进程的清理逻辑。
    ::_exit(127);   // 这是“子进程自己的退出码”；启动器会返回 125，两者不一样
}

// 三类结果，父进程必须分开处理：
//   读满一份合法记录                → 返回有值的 optional
//   一上来就是 EOF                  → nullopt，表示没收到失败报告
//   半截记录后 EOF／读失败／字段非法 → 抛异常
std::optional<LaunchError> read_launch_error(int fd) {
    LaunchError report{};
    auto* bytes = reinterpret_cast<char*>(&report);
    std::size_t received = 0;
    // 同样是字节流：不能假定一次 read 就能收齐一份结构体。
    while (received < sizeof(report)) {
        const auto count = ::read(fd, bytes + received, sizeof(report) - received);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
        } else if (count == 0) {
            // 0 就是 EOF，只有“所有写端都关闭、已写数据也读尽”时才出现。
            // 写端有两个潜在持有者：子进程（exec 成功后由 FD_CLOEXEC 关掉）
            // 和父进程自己（fork 后立刻 reset 关掉）。
            // 少关任何一个，这里就会永远阻塞——课文里那个最容易踩的坑。
            if (received == 0) {
                return std::nullopt;   // 一个字都没读到：没有报告
            }
            throw std::runtime_error("启动错误报告不完整");   // 读到一半就断了
        } else if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(),
                                    "读取启动错误报告失败");
        }
    }
    // 字段校验。虽然是同一个程序的两个副本，也不能假定对面一定守约：
    // 子进程可能写到一半被信号打死，父进程照样会读到一个“满长度”的垃圾记录。
    // errno 永远是正数，所以 code <= 0 说明这不是一份真实报告；
    // stage 只有两个合法值，零初始化留下的 0 会被这里拦下。
    if ((report.stage != LaunchStage::signals &&
         report.stage != LaunchStage::exec) || report.code <= 0) {
        throw std::runtime_error("启动错误报告格式不正确");
    }
    return report;
}

// status 是内核编码的复合值，不是裸退出码。必须**先分类，再取字段**。
int describe_status(int status) {
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        // “正常退出”只说明走的是 return/_exit 这条路，不等于业务成功——
        // --exit-code 7 也落在这一支里。
        std::cout << "子进程正常退出，退出码：" << code << std::endl;
        return code;
    }
    if (WIFSIGNALED(status)) {
        const int signo = WTERMSIG(status);
        // 这里绝对不能调 WEXITSTATUS：被信号终止时那几个 bit 的含义完全不同
        // （低 7 位是信号编号），混用会得到一个看似合理、实则错误的值。
        std::cout << "子进程被信号终止，信号编号：" << signo << std::endl;
        // 128 + 信号编号只是本启动器选定的映射，waitpid 并没有直接给出这个数字。
        // 选它是因为 shell 汇报被信号终止的程序时也用同一个约定，两处对得上。
        return 128 + signo; // 本启动器采用的退出码映射约定。
    }
    // 理论上还有 Stopped/Continued 等状态（本课的 waitpid 不带 WUNTRACED，
    // 正常不会出现）。不认识就明确报错，不默认返回 0 把未知伪装成成功。
    throw std::runtime_error("收到非预期的子进程状态");
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            // 125 是本启动器自己的“参数／内部错误”码。
            // 它和目标程序的退出码在同一数域，会撞车（对方也可能返回 125），
            // 所以最终仍要结合日志判断到底发生了什么。
            std::cerr << "用法：process_launcher 程序路径 [程序参数...]\n";
            return 125;
        }

        // 父进程忽略 SIGINT。
        // 从交互终端以前台方式运行时，fork 出来的孩子通常仍在同一个前台进程组，
        // Ctrl+C 由终端同时通知父子两者。父进程若也被打死，
        // 就没人负责回收孩子、没人报告退出状态了。
        //
        // 注意这不是“转发”：孩子收到的那份 SIGINT 是终端直接发的，
        // 不是父进程转过去的。
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        // 零初始化的 sigaction：sa_flags 等字段全为 0（取默认行为），
        // 不必手动逐个填写；sa_mask 则必须显式清空。
        check_call(::sigemptyset(&ignored.sa_mask), "初始化信号动作失败");
        check_call(::sigaction(SIGINT, &ignored, nullptr),
                   "设置父进程 SIGINT 动作失败");

        // SIGCHLD 设成 SIG_DFL。注意这**不是“忽略”**：
        // 若把 SIGCHLD 设成 SIG_IGN，内核会在子进程退出时直接丢弃它的退出状态，
        // 之后的 waitpid 只会得到 ECHILD，本课要的状态就永远拿不到了。
        struct sigaction defaults{};
        defaults.sa_handler = SIG_DFL;
        check_call(::sigemptyset(&defaults.sa_mask), "初始化默认信号动作失败");
        check_call(::sigaction(SIGCHLD, &defaults, nullptr),
                   "设置子进程回收方式失败");

        // 本课没有装 SIGCHLD 回调，等待由阻塞式 waitpid 承担。
        // “收到停止信号”和“孩子已经退出”是两类完全不同的信息。

        // 预先备好一份“要解除屏蔽的信号集合”，交给子分支用。
        sigset_t child_unblock;
        check_call(::sigemptyset(&child_unblock), "初始化子进程信号集合失败");
        check_call(::sigaddset(&child_unblock, SIGINT), "添加 SIGINT 失败");
        check_call(::sigaddset(&child_unblock, SIGTERM), "添加 SIGTERM 失败");

        // pipe 通过数组**输出**两个 fd，自己只返回 0/-1。
        // 读端用值对象、写端用 optional，因为写端要在 fork 之后提前关闭。
        int pipe_fds[2];
        check_call(::pipe(pipe_fds), "创建启动错误管道失败");
        net::SocketFd read_end(pipe_fds[0]);
        std::optional<net::SocketFd> write_end(std::in_place, pipe_fds[1]);
        // FD_CLOEXEC 不是“现在关”，而是“exec 成功那一刻自动关”，
        // 它也不阻止 fork 继承这个描述符。
        // 必须设：exec 之后子进程就是目标程序了，目标程序不该继续持有
        // 这条错误管道的写端，否则父进程永远等不到 EOF。
        // （用 pipe2 能在创建时原子地带上 O_CLOEXEC，本课用的是老接口 pipe。）
        check_call(::fcntl(write_end->get(), F_SETFD, FD_CLOEXEC),
                   "设置启动错误管道关闭标志失败");

        // fork 会把用户态缓冲区的副本一起复制过去。若此刻缓冲里还有
        // 没落盘的数据，父子各自刷一次就会重复输出，所以先刷干净，
        // 让两边都从空缓冲开始。
        // 配套的规矩是：子分支绝不刷新旧缓冲（_exit 天然不刷）。
        std::cout << std::flush;
        std::cerr << std::flush;
        const pid_t child_pid = ::fork();
        check_call(child_pid, "创建子进程失败");   // fork 失败同样是 -1 + errno

        if (child_pid == 0) {
            // ——— 子分支 ———
            // 这个 0 是 fork 的返回值，不是子进程的 PID。真正的 PID 只有父分支
            // 知道（就是父分支里那个 child_pid）；子进程想知道自己的号，
            // 得自己 getpid()。

            // 子进程不需要读端，必须手动关：子进程随后要么 exec、要么 _exit，
            // **不会**执行父进程那些 RAII 对象的析构，没人替它关。
            // （父子各自持有的是描述符副本，关掉自己的不影响对方那份。）
            ::close(read_end.get());
            // 取裸 fd，而不是继续依赖 write_end 对象：这条分支不会跑析构，
            // 别让自己误以为有 RAII 兜底。
            // 更不能在这里 reset()——那会把唯一的写端关掉，报告就发不出去了。
            const int error_fd = write_end->get();

            // 恢复信号设置，然后才 exec。
            // 忽略动作和屏蔽集合都会从父进程继承下来，而 SIG_IGN 这类动作
            // **跨 exec 保留**：不恢复的话，目标程序会莫名其妙地对 Ctrl+C 免疫。
            // 屏蔽集合同样跨 exec 保留，所以要显式 UNBLOCK。
            //
            // 这里用 sigprocmask 而不是上一课的 pthread_sigmask：
            // 启动器是单线程，fork 出来的子分支也只有一个线程；
            // sigprocmask 的返回约定是 -1 + errno，所以能走 check_call 的 < 0 判断。
            // 同一个功能、两套错误约定，正是上一课讲过的那件事。
            if (::sigaction(SIGINT, &defaults, nullptr) < 0 ||
                ::sigaction(SIGTERM, &defaults, nullptr) < 0 ||
                ::sigprocmask(SIG_UNBLOCK, &child_unblock, nullptr) < 0) {
                child_failed(error_fd, LaunchStage::signals, errno);
            }
            // execv 成功就**不会返回**：PID 不变，但程序映像已经换成目标程序，
            // 下面那行永远执行不到。返回即失败，errno 紧接着取走。
            ::execv(argv[1], argv + 1);
            child_failed(error_fd, LaunchStage::exec, errno);
        }

        // ——— 父分支 ———
        // 这一行只由父进程执行：子分支要么已经 exec 走、要么已经 _exit，
        // 都不会再回来。所以这里 child_pid 一定是正的真实 PID，
        // 不会是子分支里的那个 0。
        ChildProcess child(child_pid);
        // 父进程也必须关掉自己的写端。漏了这步就会死锁：
        // 就算子进程已经 exec 成功关掉了它那份，管道里仍有一个活着的写端，
        // 父进程永远读不到 EOF，就会永远卡在下面的 read_launch_error。
        write_end.reset();
        std::cout << "启动器 PID：" << ::getpid()
                  << "，子进程 PID：" << child_pid << std::endl;

        // 阻塞式读取。本课是“单子进程、顺序启动”，父进程就停在这儿等。
        // 要做到“边等边处理停止请求和期限”，需要 WNOHANG 加循环——那是 10-3。
        const auto launch_error = read_launch_error(read_end.get());
        if (launch_error) {
            const char* stage = launch_error->stage == LaunchStage::exec
                ? "execv" : "信号设置";
            std::cerr << "子进程启动失败，阶段：" << stage
                      << "，原因："
                      << std::system_error(launch_error->code,
                                           std::generic_category()).what()
                      << std::endl;
        } else {
            // 措辞刻意保守：EOF 只表示“没有剩余写端和数据”，它也可能来自
            // “子进程在报告之前就被信号打死”，或者“动态加载器失败”（那时 exec
            // 其实已经完成了）。所以不能把“没有坏消息”写成“启动成功”。
            // 要确认服务端真的就绪，只能连上去看它自己的监听提示。
            std::cout << "启动错误通道已关闭，未收到失败报告；等待子进程结束"
                      << std::endl;
        }

        // 有报告也仍然要等：子进程是用 _exit(127) 结束的真实进程，必须回收。
        const int status = child.wait();
        const int child_code = describe_status(status);
        // 启动失败优先于子进程的退出码：execv 失败时子分支的退出码是 127，
        // 照搬的话调用者会误以为目标程序“正常返回了 127”——
        // 正是本课要区分的那两种情况。
        const int launcher_code = launch_error ? 125 : child_code;
        std::cout << "子进程已回收，启动器退出码：" << launcher_code << std::endl;
        return launcher_code;
    } catch (const std::exception& error) {
        // 参数错误、系统调用失败、报告格式不对，都归到这里。
        // 此时 ChildProcess 的析构会在栈展开途中兜底回收孩子。
        std::cerr << "启动器失败：" << error.what() << '\n';
        return 125;
    }
}
