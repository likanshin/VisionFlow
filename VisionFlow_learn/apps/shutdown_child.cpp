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
