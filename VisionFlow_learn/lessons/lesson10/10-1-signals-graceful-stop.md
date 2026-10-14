# 模块 10.1：Linux 信号与服务端优雅停止

> 状态：2026-10-07 已完成。学生已完成空闲和带任务停止观察，集中理解检查通过。下方保留教材与历史反馈；接续 [10-2 父子进程、程序启动与退出回收](10-2-fork-exec-wait.md)，熟悉程度已确认为了解概念但没写过。

## 1. 接续位置与本课目标

模块 1～9 已完成。09-2 的普通请求及六请求观察成功，老师随后按你的要求直接解释了五道集中检查；你明确回复“明白了，进行下一步”。这属于运行成功与解释后的理解确认，不记录成独立作答通过全部题目。

你对 Linux 信号和优雅停止的熟悉程度已明确为“完全不了解，需要从概念讲起”，本课据此展开，不重复询问。

现在运行的 vision_pool_server 在 Ctrl+C 下默认被终止。我们要让它收到停止通知后，由已有事件循环安排收尾：


```text
停止接收新连接和新任务
    ↓
关闭尚未提交任务的半截请求
    ↓
继续取回已接受任务的结果，继续发送已有响应
    ↓
未交回任务为 0，并且连接表为空
    ↓
退出事件循环，join 工作线程
    ↓
正常离开 main，释放剩余描述符，返回 0
```

本课直接修改现有线程池和 vision_pool_server，不再新建另一份大入口；修改由你亲手完成。协议、算法和 Python 客户端沿用，CMake 无需调整。老师仅准备讲义和进度记录，不配置、构建、运行或测试。

## 2. 信号到底是什么

信号是操作系统向进程或线程投递的一种事件通知。它不是 TCP 请求，不经过 VFLW 头，也不是某个 C++ 函数的普通返回值。

例如，你在运行服务端的终端按 Ctrl+C，通常由终端向前台进程组发送 SIGINT。服务端在这个前台组中，因此会收到通知；在另一个终端运行的客户端通常不属于这个前台组。

| 名称 | 本课需要理解的用途 |
| --- | --- |
| SIGINT | 终端中断请求，常由 Ctrl+C 产生 |
| SIGTERM | 请求进程终止，常用于程序管理 |
| SIGKILL | 强制终止，不能被程序捕获或阻塞，本课不拿它实现收尾 |

SIGINT 和 SIGTERM 的默认动作都是终止进程。默认终止不等同于执行 return 0，不保证按 C++ 正常栈展开运行局部对象的析构函数。操作系统会回收进程资源，但客户端请求未必得到响应，工作线程也没有完成我们安排的收尾流程。[Linux signal 文档](https://man7.org/linux/man-pages/man7/signal.7.html)

所以，“终端又出现提示符”只能说明程序已经结束，不能单凭这一点证明优雅停止。

### 2.1 PID 与此前几个编号的区别

| 标识 | 标识什么 |
| --- | --- |
| PID | 操作系统中的一个进程 |
| fd | 某个进程内的打开资源，如 socket、eventfd |
| connection_token | 本程序分配的一次连接身份 |
| request_id | 客户端给一次业务请求的编号 |

本课启动时用 getpid() 打印服务端进程号。PID 用于把 SIGTERM 发给正确进程，与请求编号 42 无关。

## 3. “阻塞信号”不是把线程睡住

本课先调用 pthread_sigmask，把 SIGINT 和 SIGTERM 加入当前线程的信号屏蔽集合。

这里的“阻塞”是说：暂时不让这些信号按通常的递送方式触发默认动作或处理函数。线程仍会继续执行 C++ 代码，不是在 pthread_sigmask 处睡眠。

信号到来后可以成为待处理信号，再由 signalfd 读取。阻塞也不等于忽略：本课仍然要处理这个停止请求。

### 3.1 为什么必须在创建工作线程之前做

屏蔽集合是每线程的状态。新线程继承创建者当时的屏蔽集合，因此先由主线程屏蔽，再构造 ImageWorkerPool，工作线程就继承同样的设置。

若先启动 worker，随后只屏蔽主线程，已有 worker 的屏蔽集合不会自动跟着改变，进程收到停止信号时仍可能在未屏蔽的线程上触发默认终止。[pthread_sigmask 文档](https://man7.org/linux/man-pages/man3/pthread_sigmask.3.html)

本课不向某一条 worker 定向发送停止信号；使用 Ctrl+C 或发送给服务端进程 PID 的 SIGTERM。

### 3.2 pthread_sigmask 的参数与错误处理


```cpp
const int mask_error = ::pthread_sigmask(SIG_BLOCK, &stop_mask, nullptr);
if (mask_error != 0) {
    throw std::system_error(mask_error, std::generic_category(),
                            "阻塞停止信号失败");
}
```

SIG_BLOCK 表示在现有屏蔽集合上增加 stop_mask 中的信号。第二个参数指向我们刚建立的集合。第三个参数如果不为空，可接收旧集合；本课的独立服务端会一直保持该设置直到退出，因此传 nullptr。

它成功返回 0，失败直接返回错误编号，不能用 check_call(result) 那种只检查负值并读取 errno 的方式处理。

sigemptyset 初始化空集合，sigaddset 分别加入 SIGINT、SIGTERM。它们的错误返回方式与 pthread_sigmask 不同，所以示例分别处理。

本课的主程序不在退出前重新放开这些信号，避免刚恢复默认递送时又被待处理停止信号打断收尾。若以后把这段逻辑放入可返回给调用方的库函数，则需重新设计旧屏蔽集合的保存与恢复；不要把本课 main 的生命周期约定直接套过去。

## 4. 用 signalfd 把信号接入 epoll

上一课的 eventfd 把“工作线程有结果”转换为 epoll 能看到的可读事件。本课的 signalfd 把“有指定信号待处理”接入同一套事件循环。


```cpp
const int stop_fd = ::signalfd(
    -1, &stop_mask, SFD_NONBLOCK | SFD_CLOEXEC);
```

第一个参数 -1 表示创建新描述符；stop_mask 指定要接收的信号；SFD_NONBLOCK 使没有信号可读时返回 EAGAIN；SFD_CLOEXEC 控制以后执行新程序时的描述符继承。创建成功返回 fd，失败返回 -1。

读取的对象是 signalfd_siginfo，其中 ssi_signo 给出信号编号。我们使用 sizeof(info) 作为缓冲长度，不把它当成 eventfd 的 8 字节计数器，也不解析成网络协议。读成功会消费相应待处理信号。[signalfd 文档](https://man7.org/linux/man-pages/man2/signalfd.2.html)

本课不安装异步信号回调。在 signalfd 被读取后，begin_stop 是由 main 的正常事件循环调用的，因此可以使用已有连接表、队列方法和日志。

### 4.1 三种特别的 epoll token

| token | 对象 |
| --- | --- |
| 0 | 监听 socket |
| 1 | 工作线程结果通知 eventfd |
| 2 | 停止通知 signalfd |
| 从 3 起递增 | 客户端连接 |

必须同时把 next_token 从 2 改为 3，否则第一个客户端连接会与停止通知冲突。下面的完整 main 已处理这两处。

## 5. 停止不是一句 break，而是一个状态

增加 bool stopping，初始为 false。

第一次读取到停止信号时置为 true，执行以下策略：

| 当前资源／状态 | 停止开始时的处理 |
| --- | --- |
| listener | 从 epoll 移除并真正关闭，停止接受新连接 |
| header、body | 请求尚未提交，关闭连接，不再继续收图片 |
| waiting_result | 保留连接，继续等待已接受任务的结果 |
| sending | 保留 output 和 sent，继续发送 |
| 任务队列 | close，拒绝新任务，排空已经接受的任务 |
| eventfd 与 epoll | 继续工作，直到收尾条件满足 |

这里把“已接受任务”定义为 try_submit 返回 accepted。TCP 已经建立连接，不等于业务任务已经被接受。半截请求尚未形成完整业务任务，因此本课选择在停止时关闭它；客户端可能观察到断连，不把这种情况伪装成成功响应。

再次收到停止请求，只报告已经在停止，保持同一状态，不重复释放资源，也不把第二次 Ctrl+C 定义为强制终止。普通信号可能合并，不适合用按键次数实现精确计数协议。

### 5.1 为什么不能收到信号就 break

跳出事件循环之后，没有线程继续 take_completed 或 send。虽然线程池析构可能等待 worker 算完，但算完不等于结果已经发给客户端。

本课必须让事件循环继续服务剩余结果通知和可写连接，最后才进入 join。

### 5.2 为什么两个计数都要检查


```cpp
while (!stopping || pool.outstanding() != 0 || !connections.empty())
```

这是“还需要继续循环”的条件。退出需要同时满足：


```text
stopping == true
pool.outstanding() == 0
connections.empty() == true
```

只看 outstanding 不够：结果可能已被取回，计数变成 0，但 Connection::output 还有字节没发完。

只看连接表也不够：连接可能已经超时关闭，worker 仍在计算；它稍后返回的过期结果仍需取回、释放额度并丢弃。

未收到停止请求时，即使当前没有连接和任务，也应该继续监听，因此需要 !stopping 这一项。

## 6. 修改文件与位置

本课只由你修改以下三个现有文件：

| 文件 | 精确改动 |
| --- | --- |
| include/image_worker_pool.hpp | 在 public 的 check_health 声明之后增加两个接口 |
| src/image_worker_pool.cpp | 在命名空间内、构造函数之前增加 stop_accepting 定义 |
| apps/vision_pool_server.cpp | 补 include、增加 stop_token、插入两个函数、替换完整 main |

下面列出的新增／替换代码全部要写入对应位置。其他算法、网络收发、已学队列和客户端逻辑保留。无需新建 CMake 目标或 Python 观察脚本。

## 7. 给线程池增加两个事件循环接口

### 7.1 修改 include/image_worker_pool.hpp

在 public 区域找到：


```cpp
    void check_health() const;
```
紧接其后插入：

```cpp
    void stop_accepting();
    std::size_t outstanding() const noexcept { return outstanding_; }
```

stop_accepting 只负责关闭任务入口，不 join 线程。outstanding 返回当前尚未交回的任务数，不是线程数，也不是连接数。

outstanding_ 仍只有事件循环读写，所以 getter 不加锁；const 表示不修改池对象，noexcept 表示这个简单取值不抛异常。不要从其他线程调用它来猜测是否“刚好清空”。

### 7.2 修改 src/image_worker_pool.cpp

在 namespace visionflow::jobs { 之后、现有构造函数定义之前插入：


```cpp
void ImageWorkerPool::stop_accepting() {
    tasks_.close();
}
```

复用 09-1 的 close 语义：拒绝新任务、唤醒空闲 worker、允许取完队列中已有任务。这个方法没有等待线程结束，调用后事件循环可以马上继续收尾。

已有析构函数仍保留 tasks_.close(); join_workers();。close 可以重复调用，所以主动关入口与最终析构不冲突。不要在 stop_accepting 里直接 join，否则会提前阻塞事件循环。

## 8. 给服务端补 include 与 token

在 apps/vision_pool_server.cpp 顶部的 include 区域补充以下头文件，不删除已有 include：


```cpp
#include <csignal>
#include <optional>
#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>
```

csignal 提供信号常量；optional 用于允许 listener 提前析构；pthread.h 声明 pthread_sigmask；sys/signalfd.h 提供 signalfd 和记录类型；unistd.h 声明 read、getpid。

在现有：


```cpp
constexpr std::uint64_t completion_token = 1;
```
后面增加：

```cpp
constexpr std::uint64_t stop_token = 2;
```

## 9. 插入读取信号和开始停止的函数

在文件接近末尾，找到 parse_demo_delay 的完整函数。它后面是：


```cpp
} // namespace

int main(int argc, char* argv[]) {
```

把下面两个完整函数插入 **} // namespace 之前**，仍在匿名命名空间内。此时 check_call、would_block、close_connection 等已有函数都已经定义。


```cpp
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
```

## 10. 逐段理解两个函数

### 10.1 take_stop_signal 的输入与输出

参数 signal_fd 是 signalfd 的描述符。返回 0 表示本次没读到停止请求；返回 SIGINT 或 SIGTERM 表示读到了对应请求。

每轮最多读取 16 条记录，避免在持续收到信号时无限占住事件循环。仍有待处理记录时，LT epoll 会继续报告可读，下一轮再取。

read 成功读到一整份 info 时查看 ssi_signo；EINTR 重试；EAGAIN/EWOULDBLOCK 表示当前读完，带着已观察到的结果返回。这里每次只给 read 一份结构体大小的空间，所以正常成功返回恰好等于 sizeof(info)。

如果一次读到两种停止请求，本函数只保留本批遇到的第一个编号用于日志，状态转换仍只需一次，不需要按信号次数执行多遍 close。

### 10.2 begin_stop 的参数

| 参数 | 用途 |
| --- | --- |
| signo | 日志中显示这次是 SIGINT 还是 SIGTERM |
| epoll_fd | 移除监听和需要关闭的连接 |
| listener 引用 | 关闭并清空 main 中持有的监听对象 |
| connections 引用 | 删除未提交任务的连接，保留待结果／发送连接 |
| pool 引用 | 关闭任务入口，继续让已有任务完成 |
| stopping 引用 | 保存跨事件循环迭代的停止状态 |

函数通过引用修改 main 的实际对象，不能把 stopping 按值传入，否则函数外的状态不会变化。

### 10.3 为什么改成 optional<SocketFd>

原来的 SocketFd 只能在离开作用域时析构。现在需要在 main 继续运行期间，先把 listener 关闭，但保留 epoll、工作线程和已连接 socket。

optional 可以处于“里面有一个 SocketFd”或“里面没有对象”两种状态：


```cpp
std::optional<net::SocketFd> listener(
    std::in_place, ::socket(AF_INET, SOCK_STREAM, 0));

// 有对象时：
listener->get();

// 停止时：
listener.reset();
```

std::in_place 表示直接在 optional 内构造 SocketFd。箭头访问里面的对象，get() 再返回 fd。reset 销毁里面的 SocketFd，从而调用 close，并使 optional 变为空。

不要直接 ::close(listener->get()) 却保留原 SocketFd 中的整数：之后析构会再次 close，若 fd 整数恰好已被复用，就可能关闭其他资源。optional.reset 让资源所有权和对象状态一起更新。

只从 epoll 中 DEL 也不等于关闭监听 socket，所以代码先移除关注，再 reset。

### 10.4 删除连接时为什么先 ++it

close_connection 会从 connections 表中 erase 当前项，使当前项迭代器失效。代码先记下 token、phase，再把 it 前移，随后删除旧项。

waiting_result 与 sending 不删除。尤其 sending 可能是一份已经准备好的 busy 响应，也应该尽量发完，不因停止就丢弃。

本课保留此前的网络空闲和处理超时规则；优雅停止表示按约定收尾，不保证网络已断开的客户端仍能收到响应。

## 11. 替换完整 main

在 apps/vision_pool_server.cpp 中，从 int main(int argc, char* argv[]) { 开始到文件末尾，用下面整个 main 替换。文件前面的算法、收发函数以及刚插入的两个函数保留。

这段 main 已包含信号屏蔽、signalfd、optional listener、token 调整和收尾条件，不要再在旧 main 中叠加另一套初始化。


```cpp
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
```

## 12. main 中的先后关系

### 12.1 初始化顺序


```text
解析观察参数
→ 建立 SIGINT/SIGTERM 集合
→ pthread_sigmask 阻塞这些信号
→ 创建 signalfd
→ 创建 listener 和 epoll
→ 创建 ImageWorkerPool（工作线程继承屏蔽集合）
→ 登记完成通知
→ 进入事件循环
```

先阻塞信号，再启动工作线程，是这里最容易遗漏的顺序要求。即使信号在创建 signalfd 前、阻塞之后到来，也可以保持待处理，随后读取。

我们沿用现有 Threads::Threads 链接支持；没有增加新的第三方库。源文件没变、目标没变，所以 CMake 无需改动。

### 12.2 为什么先检查停止，再遍历网络事件

一次 epoll_wait 可能同时返回停止事件、监听事件和客户端事件，数组顺序没有“停止通知一定排在最前”的保证。

本课先查看停止描述符是否报告错误，再非阻塞读取停止信号，必要时调用 begin_stop；然后才处理本批网络事件。即使本批数组还有 listener 的旧事件，也会因 stopping 为 true 或 listener 为空而跳过。

已经关闭的半截请求也可能在本批数组里留有事件，原有 connections.find 找不到就 continue 的逻辑会处理它们。

take_stop_signal 每轮都会尝试读取，即使本批没有显式列出它；因为使用非阻塞 fd，没有信号时立即得到 EAGAIN。这也能处理在 epoll 返回附近新到来的信号。但停止开始的时刻仍是事件循环实际读到并处理请求的时刻，不是键盘按下那一纳秒。

### 12.3 为什么停止期间仍保留完成事件

任务队列关闭后，排队任务仍被 worker 取走。每条结果发布后，worker 继续写 eventfd；事件循环执行 apply_completed，把有效结果交给 Connection::output，过期结果则丢弃。

这一阶段 outstanding_ 逐步下降。任务已经全部交回时，某些响应可能还没发完，所以还需要继续处理 EPOLLOUT。

### 12.4 花括号怎样保证“已回收”日志真实

main 中有一对额外花括号包住 connections、pool 和整个循环：


```cpp
{
    Connections connections;
    jobs::ImageWorkerPool pool(make_response, demo_delay);
    // 完整事件循环在这里
} // 先析构 pool 并 join，然后销毁 connections

std::cout << "任务和连接已处理完毕，工作线程已回收，服务端正常退出"
          << std::endl;
return 0;
```

pool 比 connections 后构造，因此先析构。只有 pool 的析构完成、join 返回后，程序才打印“工作线程已回收”。

这一行输出时外层 poller 和 signals 仍由 RAII 管理，随后 return 0 离开 try 作用域时自动关闭。不要把“打印退出日志”本身当作已经关完所有 fd 的证据；这里依靠明确的对象作用域收尾。

## 13. 一条请求在停止期间的完整过程

假设请求 42 已提交，工作线程正在执行 5 秒观察延迟：


```text
t0：outstanding=1，Connection 处于 waiting_result
t1：按 Ctrl+C，SIGINT 成为待处理信号
t2：signalfd 可读，事件循环读到 SIGINT
t3：stopping=true；关闭 listener；任务队列 close
t4：继续 epoll_wait，原连接仍保留
t5：worker 完成，发布结果，写 eventfd
t6：事件循环取回结果，outstanding=0，连接进入 sending
t7：分次发送完成，删除连接
t8：stopping=true、outstanding=0、连接表为空，循环退出
t9：pool 析构并 join；打印正常退出，main 返回 0
```

如果原连接在 t5 之前断开或超时，结果仍需要在 t6 被取回并释放额度，只是随后按过期 token 丢弃。退出条件不会因为连接先消失，就假装后台任务也已经消失。

## 14. 本课没有承诺强制停止期限

此前的 30 秒期限只关闭等待结果的连接，不能强行中断 OpenCV 调用。假如某个任务永不返回，outstanding 仍不为 0，本课程序可能一直处于 stopping。

同样，重复 Ctrl+C 只提醒“已经在停止”，不会在某个次数后强杀。

这一边界是本课的明确选择：先实现合作式、有序收尾。后续进程监管再讨论由外部管理者设置宽限期、必要时终止进程及回收状态；不在本课把 join、close 或信号屏蔽说成线程强制取消机制。

## 15. 由你执行的集中实践

### 15.1 构建当前目标

完成上述三个文件的修改后，在学习工程中执行：


```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake --build build --target vision_pool_server -j2
```

沿用此前已配置的目标，本次 CMake 没有变化，不需要为了本课额外重新配置。

### 15.2 空闲时停止

终端 A 启动：


```bash
./build/vision_pool_server
```

启动日志现在应多一行“服务端 PID：实际数字”。没有客户端时按一次 Ctrl+C，预期看到类似：


```text
收到 SIGINT，开始停止；未交回任务：0，当前连接数：0
停止接收新连接和新任务，继续处理已有任务与响应
任务和连接已处理完毕，工作线程已回收，服务端正常退出
```

程序返回终端提示符后，立即执行：


```bash
echo $?
```

预期为 0。不要在这之前运行其他命令，否则读到的将是其他命令的退出码。

### 15.3 有任务时停止

终端 A 再次启动，拉长观察窗口：


```bash
./build/vision_pool_server --demo-delay-ms 5000
```

终端 B 使用已有脚本：


```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
python3 python/pool_observe.py
echo $?
```

等终端 A 中这六个请求都出现“已提交工作线程”或“被拒绝，返回 busy”，在已接受任务尚未全部完成时，按一次 Ctrl+C。这样本次集中观察不混入尚未提交的半截请求。只在服务端终端按键，让客户端继续等响应。

本次重点不是复测固定的 4 个 ok、2 个 busy，而是观察：

- “开始停止”日志的未交回任务数大于 0，说明确实在有任务时发起了停止。
- 这行之后仍有工作线程交回结果、连接响应发送完成的日志。
- 客户端能收到已接受任务的响应或已准备好的 busy，不因正常停止流程立即全部断开。
- 最后才出现“工作线程已回收，服务端正常退出”，服务端退出码为 0。
- 请求接受顺序、工作线程分配及 ok/busy 数量仍受调度影响，不要求逐字相同。

终端 A 回到提示符后也立即执行 echo $?。5 秒／10 秒附近的返回时间包含人为延迟，不是算法速度或严格的停止时限。

如果按键时所有任务已经完成，这次只能作为空闲停止证据；先贴实际日志，不把不存在的“带任务排空”记作已验证。如果提前停止时日志出现“请求尚未提交”，那些连接按本课策略会被关闭，观察脚本可能报断连；应结合具体请求阶段分析，不能直接判定已接受的任务被丢失。

### 15.4 SIGTERM 的命令形式

本课核心观察用 Ctrl+C 即可，不强制再做一轮。若要发送 SIGTERM，需要在另一个终端使用服务端启动日志里的实际 PID。

例如，**仅当本次日志的服务端 PID 确实为 12345** 时，命令才是：


```bash
kill -TERM 12345
```

kill 在这里是发送信号的命令；命令本身完成不表示服务端已经完成收尾，应看服务端的停止与退出日志。实际使用时替换为本次服务端 PID，不照搬示例数字。[kill 命令说明](https://man7.org/linux/man-pages/man1/kill.1.html)

请集中反馈构建结果、空闲停止与带任务停止的服务端日志、客户端结果以及各自退出码。老师没有执行这些命令；以上均为代码推导的预期。

## 16. 常见现象与定位

| 现象 | 首先检查 |
| --- | --- |
| Ctrl+C 后没有任何停止日志就结束 | 是否构建运行了新入口，是否在启动 worker 之前屏蔽信号 |
| pthread_sigmask 错误没有被识别 | 是否检查返回值不等于 0，并直接使用该错误编号 |
| 第一个客户端被当成停止事件 | stop_token 是否为 2，next_token 是否改为 3 |
| 收到信号后客户端立即全断开 | 是否直接 break，是否把 waiting_result／sending 也删了 |
| 没有连接了但服务端仍未结束 | 是否还有未交回任务；关闭连接不会取消任务 |
| outstanding 已为 0 仍暂时未退出 | 是否还有 Connection::output 正在发送 |
| 停止后监听事件访问空对象 | 是否在访问 listener 之前检查 stopping 或 optional 是否有值 |
| 工作线程未回收却打印了成功退出 | 成功日志是否放在 pool 所在作用域之外 |
| 连按 Ctrl+C 没有强杀 | 本课重复停止请求是幂等提醒，没有实现二次强制退出 |
| 编译提示 stop_accepting 未定义 | 是否在 image_worker_pool.cpp 的正确命名空间内添加定义 |

## 17. 集中理解检查

完成实践后，集中回答下面五点，不逐行停下来检查：

1. pthread_sigmask 的“阻塞信号”是否会把主线程睡住？为什么必须在创建工作线程之前调用？它的错误处理为什么不能直接套 check_call？
2. eventfd 与 signalfd 分别通知什么？本课是否在异步信号回调里执行 close_connection 和打印日志？
3. 收到停止请求后，为什么 listener、半截请求、waiting_result、sending 要采用不同处理？为什么关闭队列后不能立即 join？
4. 为什么退出条件同时要求 stopping、outstanding 为 0、connections 为空？分别举出只看后两个条件之一会提前退出的情况。
5. 请求等待超过 30 秒、连接已关闭，是否保证 worker 已停止？如果计算函数永不返回，本课能否保证有限时间退出？

## 18. 当前状态与下一步

模块 1～9 已完成。10-1 熟悉程度已记录为完全不了解，讲义已准备；三个现有 C++ 文件由学生修改，课程配置、构建、运行、测试均由学生执行。老师本次仅维护教学辅助资料，没有改主要源码或 CMake。

2026-10-07（10-1 实现及空闲停止已确认，带任务停止待观察）：学生已修改三个现有 C++ 文件，老师只读核对了信号屏蔽先于 worker 创建、signalfd token=2、连接 token 从 3 起、optional listener 关闭、队列 close、联合退出条件及 join 后日志等主要路径。PID 11584 的空闲 SIGINT 停止正常，退出码 0；PID 11877 在 --demo-delay-ms 5000 下先完成六请求（201、204 busy；202、206 约 5.03 秒成功；203、205 约 10.03 秒成功），随后才按 Ctrl+C，此时未交回任务和连接数均为 0，退出码也为 0。因此两次都属于空闲停止，不能记作已验证收到停止请求后继续排空任务。客户端退出码及本次构建输出未展示，不倒填。下一步建议用原 vision_client 发单个请求 42，在服务端显示“已提交工作线程”后、结果交回前按 Ctrl+C，观察停止开始时未交回任务大于 0，以及之后的结果、响应、回收日志和两端退出码；不要求重跑已通过的空闲停止。带任务停止和集中理解检查尚未完成，SIGTERM 可选实验不催补。老师未修改主要源码，未配置、构建、运行或测试。

只读核对另发现 vision_pool_server.cpp 顶部两条误导注释：发送阶段仍由 expire_connections 应用 10 秒无收发进展期限，并非只由内核 TCP 超时控制；token 0 是 listener、1 是完成通知、2 是停止信号。老师仅指出供学生校正，保留原代码和注释，未代改。

2026-10-07（10-1 带任务停止观察通过，集中理解检查待完成）：学生再次运行 vision_pool_server --demo-delay-ms 5000，PID 15151，使用六请求脚本。请求 203、206 先返回 busy；收到 SIGINT 时日志明确为未交回任务 4、当前连接数 4。进入停止状态后，请求 201、202、204、205 的结果继续交回，四条连接均记录响应已发送后关闭，最后才打印工作线程已回收、服务端正常退出。客户端对应收到 201、202 约 5.01 秒的 ok，204、205 约 10.02 秒的 ok，汇总成功 4、忙碌 2。由此确认停止后仍处理了四个已接受任务的完成结果并发送响应；时间包含人为延迟，不作为算法或停止时限数据。本轮未展示两端 echo $?，不把此前空闲停止的退出码 0 当作本轮退出码；也未验证 SIGTERM、半截请求关闭、任务卡住等其他分支，不要求补跑。结合此前空闲停止两次退出码 0，10-1 实现、空闲及带任务停止的运行观察已完成，下一步只接续五道集中理解检查，不要求再跑单请求或六请求停止实验。老师仅维护教学记录，未修改源码、配置、构建、运行或测试。

2026-10-07（10-1 理解检查通过）：学生正确回答信号屏蔽不会使主线程睡眠、新线程继承屏蔽集合、eventfd 与 signalfd 的通知用途、关闭后的任务仍有结果需处理、任务数归零时可能仍有未发完响应、连接表为空时可能仍有后台任务，以及连接超时不取消 worker、计算不返回则不能保证有限时间退出。老师补充精确细节：pthread_sigmask 成功返回 0、失败直接返回错误编号，不套用“负值加 errno”的 check_call，也不把所有 pthread_* 函数一概而论；stop_accepting 只 close、不立即 join，让事件循环继续取结果和发送；listener 和未提交的半截请求停止接入，waiting_result/sending 保留；stopping 防止正常空闲时退出。结合此前空闲与带任务停止的实际输出，10-1 实践和理解检查通过。老师补充的细节不记作学生已独立完整复述，不重复要求回答或重跑。下一步 10-2 父子进程、程序启动与退出回收／监管，先确认相应熟悉程度；讲义和实现尚未生成。老师仅更新教学资料，未修改源码、配置、构建、运行或测试。
