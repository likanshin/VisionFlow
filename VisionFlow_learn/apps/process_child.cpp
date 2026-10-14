// 模块 10.2 的示例子进程：由 process_launcher fork + execv 启动，
// 用来制造三种可预期的结局（正常退出 0、正常非零退出、被信号终止），
// 让父进程把 waitpid 的 WIFEXITED / WIFSIGNALED 两条分支都走一遍。
// 它自己不创建子进程，不联网，也没有工作线程。
#include <csignal>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>   // getpid、getppid、raise

int main(int argc, char* argv[]) {
    try {
        // 两个模式互斥，各用一个变量表示，而不是塞进一个枚举或哨兵值：
        // 不带参数是合法默认路径（正常退出、退出码 0），
        // 这样就不用额外记录“用户到底选没选”。
        int exit_code = 0;
        bool terminate_by_signal = false;

        // argc == 3 是精确匹配，不是 >= 3。多给的参数宁可报用法错误，
        // 也不要静默忽略——示例程序里能被看见的错误，好过沉默的错误。
        if (argc == 3 && std::string(argv[1]) == "--exit-code") {
            const std::string text = argv[2];
            // 第一步：只校验“形状”——必须是 1～3 位纯数字。
            // 空串会在 stoi 里抛；超过 3 位数值必然大于 255；
            // find_first_not_of 命中说明混进了 '+'、空格、字母或小数点，
            // 其中 "+7"、"7abc" 这类 std::stoi 是会接受的（甚至部分解析），
            // 所以这个校验不能甩给 stoi 代劳。
            if (text.empty() || text.size() > 3 ||
                text.find_first_not_of("0123456789") != std::string::npos) {
                throw std::invalid_argument("退出码必须为 0～255 的整数");
            }
            // 走到这里 stoi 必定不抛、也不溢出：1～3 位纯数字最大就是 999。
            exit_code = std::stoi(text);
            // 第二步：再校验“范围”，必须和形状分开判。
            // "999" 形状完全合法，只有在这里才拦得住它。
            if (exit_code > 255) {
                throw std::invalid_argument("退出码不得超过 255");
            }
        } else if (argc == 2 && std::string(argv[1]) == "--signal-term") {
            terminate_by_signal = true;
        } else if (argc != 1) {
            // 兜底：argc == 1（不带参数）是合法的默认路径，其余组合一律算用法错误。
            // 由于上面两个分支都要求参数个数精确匹配，
            // 少给值的 "--exit-code"、写反的 "xx --exit-code 7" 都会落到这里。
            throw std::invalid_argument(
                "用法：process_child [--exit-code 0到255 | --signal-term]");
        }

        // PID 是自己，PPID 是启动器。这两行是给父进程日志做对照的：
        // 这里的 PID 应当等于启动器 fork 返回的那个数字，
        // PPID 应当等于启动器自己的 PID——这也是“子分支里 fork 返回的 0
        // 并不是子进程 PID”这件事最直接的实证。
        std::cout << "示例子进程：PID=" << ::getpid()
                  << "，PPID=" << ::getppid() << std::endl;

        if (terminate_by_signal) {
            std::cout << "示例子进程准备向自身发送 SIGTERM" << std::endl;
            // raise 发给的是“正在执行它的那条线程”（等价于
            // pthread_kill(pthread_self(), SIGTERM)），不是发给父进程，
            // 也不是发给整个前台进程组。本程序单线程，所以效果等同于 kill(getpid(), ...)。
            // 正常路径下进程就地终止，不会返回，下面一行执行不到。
            //
            // 它能被杀死的前提是：启动器在 execv 之前已经替子进程
            // 把 SIGTERM 恢复成 SIG_DFL 并解除了屏蔽（讲义 11.2）。
            // 那是父进程的准备工作，不是本程序自己做的。
            ::raise(SIGTERM);
            // 这行是哨兵，正常永远不该执行到。它同时兜住两种情况：
            // SIGTERM 的动作被改坏（被忽略或仍被屏蔽），或者 raise 本身失败。
            // 有它在，程序不会悄悄返回 0 把失败伪装成成功——见讲义 7.2。
            throw std::runtime_error("SIGTERM 未终止进程，请检查信号设置");
        }
        // 正常退出路径：会跑 C++ 栈展开、刷新 stdout 缓冲，
        // 然后内核取 exit_code 的低 8 位作为退出状态交给父进程的 waitpid。
        // 这正是要把取值范围限制在 0～255 的原因：更大的数会被截断，
        // 父进程提取到的值就和你输入的对不上了。
        std::cout << "示例子进程准备正常退出，退出码：" << exit_code << std::endl;
        return exit_code;
    } catch (const std::exception& error) {
        // 参数用法错误、以及上面那个“SIGTERM 没生效”的哨兵，都从这里出去。
        // 统一返回 1：父进程只看退出码时无法把它和 --exit-code 1 区分开，
        // 要靠日志（stderr 那行）来分辨——这本身就是本课要你体会的一点。
        std::cerr << "示例子进程失败：" << error.what() << '\n';
        return 1;
    }
}
