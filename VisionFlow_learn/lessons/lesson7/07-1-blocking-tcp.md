# 第七模块：第一次编写 TCP 客户端与服务端

## 1. 本课目标、当前进度与阅读方法

你了解 TCP、socket 大概用来做什么，但还没有完整写过网络程序。这一课从现有代码出发，讲清楚：连接怎样建立、字节怎样进入缓冲区、怎样把图片请求变成处理结果，以及两种语言怎样遵守同一份 VFLW 格式。

本课完成一次完整往返：Python 读取 JPEG 文件，发送图片字节；C++ 接收并处理图片，再发送 JSON；Python 接收并显示结果。服务端只监听同一个 WSL 中的 `127.0.0.1:19090`，接受一次连接、处理一个请求，发送响应后退出。连续请求、非阻塞和并发在后面学习。

**2026-10-01 只读核对：四个网络源码文件已经存在，主要实现与上一版讲义一致。你不用为了阅读新版而删除或重写它们。** 下面保留每个文件的完整代码，供你对照；紧接着按执行顺序拆开解释。老师这次只改讲义和进度记录，没有配置、构建、运行或测试程序；模块 7 的实践结果与理解情况仍待你反馈。

建议按顺序阅读第 2～7 节，打开对应文件，把解释和实际语句对起来。每节的短代码都是从完整文件摘出的阅读片段，**不要把它们再次追加到源码末尾**。如果某个文件尚未写入，才按对应的“完整内容”填写。理解检查集中在讲义最后，不需要每读几行就停下来回答。

## 2. 先把网络调用放进实际过程

### 2.1 同一张图片，在各阶段是什么

以前的 CLI 从文件路径读图；现在文件在客户端读取，服务端收到的是文件内容：

```text
Python 客户端内存                         C++ 服务端内存
samples/frame.jpg
    ↓ read_bytes()
JPEG 压缩字节
    ↓ 加上 24 字节 VFLW 头
一条请求：[消息头][JPEG 字节]
    ↓ sendall() ─────── TCP ───────────→ recv_exact()
                                        ↓ decode() 解释消息头
                                      body：JPEG 压缩字节
                                        ↓ cv::imdecode()
                                      image：解码后的像素矩阵
                                        ↓ processor->process()
                                      result：ProcessResult
                                        ↓ 构造 JSON，再 dump()
                                      JSON 文本的字节
recv_exact() ←──────── TCP ─────────── send_all([消息头][JSON 字节])
    ↓ UTF-8 解码，再 json.loads()
Python 字典，最后显示结果
```

客户端不会把字符串 `samples/frame.jpg` 当成图片发出去；C++ 也不需要打开客户端的路径。`read_bytes()` 把本地文件变成字节，网络传的是这些字节。JPEG 字节不能直接当成 `cv::Mat` 的像素，需要 `imdecode()` 解码。

这里复用前面已经写好的协议和图像处理函数。第六模块的 `packet()` 只负责在内存里拼头和体，本课的 `send_all()` 才把这段内存交给操作系统发送。

### 2.2 地址、端口和描述符，分别表示什么

| 本课中的值 | 用来回答什么 | 出现在哪里 |
| --- | --- | --- |
| `127.0.0.1` | 要连接本机的哪个地址？ | C++ 绑定地址、Python 连接地址 |
| `19090` | 本机这个地址上的哪个服务入口？ | 两端约定的 TCP 端口 |
| `fd`，例如 3 或 4 | 当前进程操作哪个已经打开的资源？ | C++ 的 socket、accept、recv、send |
| `request_id=42` | 这条响应对应哪个应用请求？ | VFLW 头和结果 JSON |

`127.0.0.1` 是回环地址。本课两个终端都在同一 WSL 发行版里运行，所以连接指向同一套 Linux 网络环境；数据不需要先经过互联网或 VPN。端口 19090 是我们选定的入口，双方必须一致。[Linux IPv4 手册](https://man7.org/linux/man-pages/man7/ip.7.html)说明了 IPv4 地址与端口的表示。

文件描述符 `fd` 是 Linux 为当前进程打开的资源分配的整数编号。标准输入、标准输出、标准错误通常对应 0、1、2，套接字也会获得一个描述符。后续用这个编号告诉系统“对哪个连接读或写”。编号只在相应进程里有意义；不能把服务端的 fd 当成客户端的编号，也不能写死它一定是 3。

### 2.3 建立连接的调用顺序

```text
C++ 服务端                                      Python 客户端
socket()：创建套接字
bind()：指定本机地址和端口
listen()：让套接字进入监听状态
accept()：等待并取出一个连接  ←──────────────  create_connection() 内部连接
返回新的已连接套接字                            得到已连接 socket 对象
recv_exact()：收请求  ←──────────────────────  sendall()：发请求
处理图片
send_all()：发响应  ────────────────────────→  recv_exact()：收响应
关闭已连接套接字                                  关闭连接
关闭监听套接字
```

`socket()` 本身只是创建资源。`bind()` 说明服务入口在哪，`listen()` 让它开始接受连接；在没有客户端时，真正停下来等待的是 `accept()`。

服务端有两个不同的套接字：监听套接字负责接受连接，`accept()` 返回的已连接套接字负责与这个客户端收发。它们不是同一个整数，也不是先监听再把同一个资源“改造成连接”。[Linux accept 手册](https://man7.org/linux/man-pages/man2/accept.2.html)明确区分这两个资源。

### 2.4 “阻塞”具体发生在哪里

阻塞调用的意思是：当前线程在函数里等待条件满足，暂时不会执行下一行。它不是一个需要你手动写的忙等循环。

| 调用 | 本课什么时候可能等待 | 什么时候继续 |
| --- | --- | --- |
| 服务端 `accept()` | 还没有可接受的连接 | 有连接，或发生错误／中断 |
| 两端接收数据 | 接收缓冲区里还没有需要的数据 | 收到字节，或对端结束发送／出现错误 |
| 发送数据 | 系统发送缓冲区暂时没有足够空间 | 可以接收更多待发字节，或出现错误 |

阻塞的 `recv()` **不会因此保证一次收满你要求的长度**。有一些字节可以读时，它可能先返回这些字节。所以要同时理解两件事：“没有数据时等待”和“有数据时可能只返回一部分”。第 5 节会用数字走一遍循环。

## 3. 文件分工与动手位置

所有文件都在 `/home/rikanshin/VisionFlow/VisionFlow_learn`，不要改原项目的同名文件。

| 文件（相对学习工程根目录） | 这一层负责什么 | 这一层调用谁 |
| --- | --- | --- |
| `include/tcp_io.hpp` | 声明资源类、完整收发函数 | 提供给其他源码使用 |
| `src/tcp_io.cpp` | 关闭描述符，循环收齐／发完字节 | Linux 的 close、recv、send |
| `apps/vision_server.cpp` | 建立监听、接收请求、处理图片、回传结果 | tcp_io、protocol、processor |
| `python/vision_client.py` | 读文件、连接服务端、封装请求、解析响应 | Python 标准库 |

已有 `include/protocol.hpp`、`src/protocol.cpp` 负责 VFLW；已有 `include/processor.hpp`、`src/processor.cpp`、`src/line_utils.cpp` 负责图像处理。本课在这两部分外面加网络入口，算法参数暂时固定为 `lane_lines`、最多 3 条线段。

## 4. 套接字资源类：include/tcp_io.hpp

### 4.1 文件的完整内容

打开 `/home/rikanshin/VisionFlow/VisionFlow_learn/include/tcp_io.hpp`。以下对应整个文件，不是让你插入到其他类里面。

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace visionflow::net {

class SocketFd {
public:
    explicit SocketFd(int fd);
    ~SocketFd();

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
```

### 4.2 三个函数参数，在收发时怎样用

```cpp
void recv_exact(int fd, std::uint8_t* data, std::size_t size);
void send_all(int fd, const std::uint8_t* data, std::size_t size);
```

| 参数 | recv_exact 中的含义 | send_all 中的含义 |
| --- | --- | --- |
| `fd` | 从哪个连接读 | 向哪个连接写 |
| `data` | 把读到的字节写到哪里 | 从哪里取得要发送的字节 |
| `size` | 本次总共要收齐多少字节 | 本次总共要交给系统多少字节 |

`std::uint8_t` 在当前环境中用来保存一个字节。`std::uint8_t*` 指向一段可写的字节内存；发送参数带 `const`，因为函数只读取这段内存。`std::size_t` 适合保存容器大小和缓冲区长度。

这两个函数不理解 VFLW，也不识别图片；它们只按调用者指定的字节数工作。调用者必须先准备好至少 `size` 字节的有效内存。稍后服务端会先传 24 字节的数组，再传按头部长度建立的 vector。

### 4.3 SocketFd 包装的是哪一个资源

```cpp
explicit SocketFd(int fd);
int get() const { return fd_; }
```

构造函数接收系统返回的描述符，把它存入私有成员 `fd_`。`get()` 只返回这个整数，供 `bind()`、`recv()` 等调用使用；它没有复制套接字资源，也没有关闭它。末尾的 `const` 表示这次取编号不修改包装对象。

`explicit` 要求明确写出构造动作，例如 `SocketFd connection(accepted)`，避免某个整数在意外的位置自动变成资源对象。`SocketFd` 是我们自己写的 C++ 类，真正的 socket 由 Linux 创建；类负责管理它的使用期限。

### 4.4 为什么禁止复制

```cpp
SocketFd(const SocketFd&) = delete;
SocketFd& operator=(const SocketFd&) = delete;
```

如果两个对象都保存同一个 fd，并且都在析构时关闭它，就会造成重复关闭。Linux 还可能把已释放的编号分配给新资源，此时第二次关闭甚至会误关其他资源。这里直接让编译器拒绝复制和复制赋值。

你已经学过 RAII，本课把同一原则用于 socket：创建成功后立即交给局部对象；正常返回或异常离开作用域时，由析构函数关闭。这里没有提供移动操作，服务端直接在使用位置构造两个对象，不需要转移它们。

`namespace visionflow::net` 把网络工具的名称放在一起。服务端的 `namespace net = visionflow::net;` 是简写，之后可以写 `net::SocketFd`。它不会创建一个新的网络对象。

## 5. 收发循环：src/tcp_io.cpp

### 5.1 文件的完整内容

打开 `/home/rikanshin/VisionFlow/VisionFlow_learn/src/tcp_io.cpp`，对应完整文件如下。

```cpp
#include "tcp_io.hpp"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>

namespace visionflow::net {

SocketFd::SocketFd(int fd) : fd_(fd) {
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "获取套接字失败");
    }
}

SocketFd::~SocketFd() {
    ::close(fd_);
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
```

### 5.2 头文件和 :: 前缀分别干什么

| 头文件 | 这里提供的内容 |
| --- | --- |
| `tcp_io.hpp` | 我们自己的类和函数声明 |
| `<cerrno>` | 系统调用失败后的 errno、EINTR |
| `<stdexcept>` | runtime_error |
| `<system_error>` | system_error，把系统错误码放进异常 |
| `<sys/socket.h>` | recv、send、MSG_NOSIGNAL 等 Linux 套接字接口 |
| `<unistd.h>` | close |

`::recv()`、`::send()`、`::close()` 前面的 `::` 表示从全局命名空间找函数。我们正处于 `visionflow::net` 里面，这样写能清楚区分系统接口与我们自己定义的工具。

这些系统接口不是 C++ 标准库的网络类。当前在 WSL/Linux 编译，使用 Linux 的头文件和调用方式。

### 5.3 构造和析构：何时获得资源，何时关闭

```cpp
SocketFd::SocketFd(int fd) : fd_(fd) {
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "获取套接字失败");
    }
}

SocketFd::~SocketFd() {
    ::close(fd_);
}
```

`: fd_(fd)` 是成员初始化列表，把传入的整数保存到 `fd_`。系统的 `socket()` 和 `accept()` 成功时返回非负编号，失败时返回 -1；因此这里检查 `< 0`。

`errno` 保存这次系统失败的原因，例如资源不足。`std::generic_category()` 用来解释这种系统错误码；`std::system_error` 同时保存错误码和我们写的中文说明，外层 `catch` 中的 `error.what()` 可以显示这些信息。只有刚刚的系统调用报告失败时才根据 errno 判断，成功调用后它可能还留着旧值。

如果构造函数抛异常，包装对象没有构造成功，不会调用它自己的析构函数；这里的 fd 本来就是失败返回值，没有需要关闭的成功资源。如果构造成功，析构时调用 `close(fd_)`，释放当前进程对套接字的这个描述符。本课析构不再抛出新异常。

服务端先构造 `listener`，后构造 `connection`。离开作用域时按相反顺序销毁：先关闭连接，再关闭监听资源。如果中途抛异常，已经成功构造的对象同样会自动析构。

### 5.4 先单独看一次 recv 调用

```cpp
const auto count = ::recv(fd, data + done, size - done, 0);
```

四个实参依次是：

| 实参 | 系统这次会怎样使用 |
| --- | --- |
| `fd` | 从指定的已连接套接字读取 |
| `data + done` | 把新字节写入这次指定的起始位置 |
| `size - done` | 这次最多写入这么多字节 |
| `0` | 使用普通接收方式，没有额外标志 |

`recv()` 的返回值是有符号的字节数量，`auto` 在这里推导出的类型是系统接口的 `ssize_t`。它不是返回一段字符串，**字节已经由系统写进你提供的内存；返回值告诉你写进了多少**。

| 返回值 | 本课如何解释 | 下一步 |
| --- | --- | --- |
| `count > 0` | 本次读到了 count 个字节 | 累加 done，再看是否收齐 |
| `count == 0` | 对端的发送方向结束，已没有后续可读数据 | 未收齐就抛异常 |
| `count == -1` | 本次调用失败 | 看 errno，中断则重试，其他错误抛异常 |

返回 0 也可能来自对端仅关闭发送方向，并不总表示两个方向都不能通信。本课在需要的数据还没齐时遇到它，当前请求已无法按约定完成，因此直接报错。普通阻塞接收在“对方还没发，但连接的发送方向仍开着”时会等待，并不是立刻返回 0。[Linux recv 手册](https://man7.org/linux/man-pages/man2/recv.2.html)给出了返回值约定。

### 5.5 用 24 字节消息头，走完一次循环

```cpp
std::size_t done = 0;
while (done < size) {
    const auto count = ::recv(fd, data + done, size - done, 0);
    // 根据 count 更新进度、重试或报错
}
```

假设调用方需要收齐 24 字节，系统分三次返回 10、8、6。数字是解释用的示例，不是一次实际运行记录：

| 第几次调用 | 调用前 done | 写入位置 | 这次最多接收 | 假设返回 count | 更新后的 done |
| --- | --- | --- | --- | --- | --- |
| 1 | 0 | data + 0 | 24 | 10 | 10 |
| 2 | 10 | data + 10 | 14 | 8 | 18 |
| 3 | 18 | data + 18 | 6 | 6 | 24 |

```text
缓冲区下标：0 ........ 9 | 10 ..... 17 | 18 ... 23
第一次写入：前 10 字节
第二次写入：              接下来的 8 字节
第三次写入：                            最后的 6 字节
```

`data` 指向第一个字节，`data + 10` 就指向下标 10，即第 11 个字节。这里是指针运算，移动的是写入起点，不是在把字节数加进图片数据。

为什么两处都要减去／加上 done？如果第二次还从 `data` 写，就会覆盖已经收到的前 10 字节；如果起点移动了、长度仍给 24，就可能写出缓冲区边界。正确的组合是“从未填写位置开始，只允许写剩余长度”。

`done += static_cast<std::size_t>(count)` 只放在 `count > 0` 的分支内，因为此时能安全把正数转成无符号长度。若先把 -1 转成 size_t，会得到一个非常大的数，进度和边界都会出错。

当 done 等于 size，while 条件变为假，函数正常结束。它返回 `void`：成功意味着调用者提供的这段内存已经填满；失败通过异常报告。size 为 0 时不会进入循环，不过本协议要求消息体非空，正常调用会传正数。

### 5.6 EINTR 分支为什么没有额外语句

```cpp
} else if (errno != EINTR) {
    throw std::system_error(errno, std::generic_category(),
                            "接收数据失败");
}
```

执行到这里已经知道 count 是负数。`EINTR` 表示系统调用被信号中断。若 errno 是 EINTR，这个 if 不成立，不抛异常、不增加 done；到达本轮末尾后，while 重新发起接收，起点和剩余长度保持原样。

它不是忽略全部错误。连接重置等其他错误仍会抛出异常，跳到调用方的 catch。阻塞接收没有设置超时；如果对方一直保持连接却不继续发送，函数可能一直等待，超时在后续知识点处理。

### 5.7 send_all 与 recv_exact 相同和不同的地方

```cpp
const auto count = ::send(fd, data + done, size - done, MSG_NOSIGNAL);
```

发送方向也记录 done。假设共有 1000 字节要发，第一次返回 400，下一次就从 `data + 400` 继续，剩余 600 字节。这样不会重发已经被系统接受的前 400 字节。

接收是把字节写进 data；发送是从 data 读出字节。`count > 0` 表示这次有多少字节被本机系统接受用于发送，并不证明服务端程序已经处理完它们。`send_all()` 正常结束也只是完成本次向系统交付；应用是否处理成功，要根据稍后收到的响应判断。

`count == 0` 表示本轮没有进展；本课抛异常，避免在相同位置无限循环。负数仍按 EINTR 重试、其他错误报告的方式处理。

`MSG_NOSIGNAL` 是 Linux 的发送标志：向已断开的连接发送时，避免 SIGPIPE 信号直接终止进程，让程序有机会通过负返回值和 errno 报告错误。它不会让发送失败变成成功，也不负责补发。[Linux send 手册](https://man7.org/linux/man-pages/man2/send.2.html)说明了这一标志和发送返回值。

### 5.8 为什么收头时不会误吃掉后面的图片

假设客户端一次把 `[24 字节头][1000 字节体]` 都交给系统，服务端接收缓冲区里已经有完整的 1024 字节。

第一次 `recv_exact(..., 24)` 中的每次 recv 都只请求“还缺的头部字节数”，总共只取 24 字节。其余 1000 字节留在套接字接收缓冲区，下一次 `recv_exact(..., 1000)` 再取。

应用消息的边界由“固定头长 + 头中声明的体长”确定。我们不依靠 TCP 的分段方式、一次 send 对应一次 recv，也不依靠遇到换行来判断 JPEG 是否结束。

## 6. C++ 服务端：apps/vision_server.cpp

### 6.1 文件的完整内容与接下来要读的位置

打开 `/home/rikanshin/VisionFlow/VisionFlow_learn/apps/vision_server.cpp`。下面对应整个文件。第 6.2～6.13 节按这份代码真正执行的顺序解释。

```cpp
#include "processor.hpp"
#include "protocol.hpp"
#include "tcp_io.hpp"

#include <array>
#include <cerrno>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

namespace protocol = visionflow::protocol;
namespace net = visionflow::net;

void check_call(int result, const char* message) {
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), message);
    }
}

int main() {
    try {
        net::SocketFd listener(::socket(AF_INET, SOCK_STREAM, 0));
        const int reuse = 1;
        check_call(::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                               &reuse, sizeof(reuse)), "设置地址复用失败");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(19090);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check_call(::bind(listener.get(),
                          reinterpret_cast<const sockaddr*>(&address),
                          sizeof(address)), "绑定监听地址失败");
        check_call(::listen(listener.get(), 1), "开始监听失败");
        std::cout << "正在监听 127.0.0.1:19090，等待一次图片请求……"
                  << std::endl;

        int accepted;
        do {
            accepted = ::accept(listener.get(), nullptr, nullptr);
        } while (accepted < 0 && errno == EINTR);
        net::SocketFd connection(accepted);
        std::cout << "客户端已连接\n";

        std::array<std::uint8_t, protocol::header_size> header_bytes{};
        net::recv_exact(connection.get(), header_bytes.data(),
                        header_bytes.size());
        const auto header = protocol::decode(header_bytes.data(),
                                              header_bytes.size());
        if (header.type != protocol::Type::image) {
            throw std::runtime_error("服务端只接受图片请求");
        }
        std::vector<std::uint8_t> body(header.payload_length);
        net::recv_exact(connection.get(), body.data(), body.size());

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
        } catch (const std::exception& error) {
            response_type = protocol::Type::error;
            response = {
                {"request_id", header.request_id},
                {"status", "error"},
                {"code", error_code},
                {"message", "图片请求处理失败"}
            };
            std::cerr << "图片请求处理失败：" << error.what() << '\n';
        }

        const std::string json_text = response.dump();
        constexpr std::size_t max_response = 64U * 1024U;
        if (json_text.size() > max_response - protocol::header_size) {
            throw std::runtime_error("响应内容超过允许上限");
        }
        const std::vector<std::uint8_t> response_body(json_text.begin(),
                                                        json_text.end());
        const auto message = protocol::packet(response_type,
                                               header.request_id,
                                               response_body);
        net::send_all(connection.get(), message.data(), message.size());
        std::cout << "响应已发送，本次连接处理结束\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "服务端失败：" << error.what() << '\n';
        return 1;
    }
}
```

这里三个项目头文件分别提供处理器、VFLW、网络工具。`<arpa/inet.h>` 提供本课使用的字节序转换等 IPv4 接口，`<sys/socket.h>` 提供 socket、bind、listen、accept 等。`<opencv2/imgcodecs.hpp>` 提供内存图片解码，`<nlohmann/json.hpp>` 提供 JSON。

```cpp
namespace protocol = visionflow::protocol;
namespace net = visionflow::net;
```

两个 namespace 别名只是缩短名称。例如 `protocol::decode()` 就是前一课实现的 `visionflow::protocol::decode()`，没有重新实现一份解码器。

### 6.2 check_call：把系统失败转换成 C++ 异常

```cpp
void check_call(int result, const char* message) {
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), message);
    }
}
```

本文件用它检查 setsockopt、bind、listen 的结果。这几种调用成功返回 0，失败返回 -1 并设置 errno。`message` 是调用处给出的中文说明，例如“绑定监听地址失败”。

```cpp
check_call(::listen(listener.get(), 1), "开始监听失败");
```

这行先调用 listen，再把返回值交给 check_call。成功就继续下一行；失败就抛异常，由 main 最外面的 catch 显示原因并返回 1。这样业务流程里不用为每个调用重复写一遍相同的错误分支。

socket 和 accept 成功返回的是描述符，它们直接交给 SocketFd 检查。recv、send 的返回值还要区分字节数、结束和中断，所以由收发循环单独处理。

### 6.3 socket：创建资源，此时还没有客户端

```cpp
net::SocketFd listener(::socket(AF_INET, SOCK_STREAM, 0));
```

先看里面的系统调用：

| 参数 | 本课的选择 | 具体意思 |
| --- | --- | --- |
| 地址族 | `AF_INET` | 使用 IPv4 地址 |
| 套接字类型 | `SOCK_STREAM` | 使用连接式字节流 |
| 协议 | `0` | 对这里的 IPv4 字节流使用默认协议 TCP |

不是任何场景里 SOCK_STREAM 都等于 TCP；本课 `AF_INET + SOCK_STREAM + 0` 这一组合创建 TCP 套接字。[Linux socket 手册](https://man7.org/linux/man-pages/man2/socket.2.html)说明了三个参数的职责。

假设 socket 返回 3，外面的构造就相当于 `net::SocketFd listener(3)`。这只是示例编号。随后 `listener.get()` 会返回实际编号，析构负责 close。

到这行结束，只是有了一个套接字资源；还没有绑定 19090，也没有和 Python 建立连接。

### 6.4 setsockopt：给监听套接字设置一个选项

```cpp
const int reuse = 1;
check_call(::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR,
                       &reuse, sizeof(reuse)), "设置地址复用失败");
```

setsockopt 的五个实参分别是：

| 实参 | 含义 |
| --- | --- |
| listener.get() | 修改哪个套接字 |
| SOL_SOCKET | 修改套接字这一层的选项 |
| SO_REUSEADDR | 选中“地址复用”这个选项 |
| &reuse | 选项值存放的内存地址，里面是整数 1，表示启用 |
| sizeof(reuse) | 这个选项值占多少字节 |

`&reuse` 取整数变量的地址。系统按你给的长度读取内存中的值；这里不是把指针的数值当作 1。

本课服务端会频繁退出再启动。SO_REUSEADDR 用于允许某些地址复用情形，减少因之前连接状态而导致的重新绑定问题。它不保证任何时候绑定都成功；已有服务正在监听同一地址和端口时，仍可能报地址占用。该选项也不是“允许所有客户端共享一个连接”。[Linux socket 选项手册](https://man7.org/linux/man-pages/man7/socket.7.html)说明了 SO_REUSEADDR 的限制。

### 6.5 sockaddr_in：把地址和端口填写到系统要求的结构里

```cpp
sockaddr_in address{};
address.sin_family = AF_INET;
address.sin_port = htons(19090);
address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
```

第一行创建 IPv4 地址结构，`{}` 把其成员初始化为零，随后填写真正需要的字段：

| 字段 | 填写什么 | 为什么需要 |
| --- | --- | --- |
| sin_family | AF_INET | 告诉系统这是一份 IPv4 地址 |
| sin_port | htons(19090) | 保存 16 位端口，按网络字节序表示 |
| sin_addr.s_addr | htonl(INADDR_LOOPBACK) | 保存 32 位回环地址，按网络字节序表示 |

`sin_addr` 自身是一个结构，里面的 `s_addr` 保存 IPv4 地址的整数表示，所以有两层点号。`INADDR_LOOPBACK` 是系统提供的本机回环地址常量，不是字符串。

你已经知道大小端，这里只需把知识放到具体字段上：`htons` 的 h 是 host、n 是 network，s 对应 16 位；`htonl` 对应 32 位。它们把主机表示转成接口要求的网络表示；不是让你手工翻转十进制数的数字顺序。主机本来就是大端时，转换也能得到正确结果。[字节序转换手册](https://man7.org/linux/man-pages/man3/byteorder.3.html)给出这些函数的定义。

**这份 address 是交给操作系统 bind 的地址信息，不是发给客户端的 VFLW 头。** 端口字节序在这里转换；VFLW 的请求编号、消息体长度仍由 protocol 的编码函数处理。不要把两份结构混在一起，也不用给 sin_family 调用 htons。

### 6.6 bind：把监听资源绑定到填写好的地址

```cpp
check_call(::bind(listener.get(),
                  reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)), "绑定监听地址失败");
```

三个实参分别是：哪个套接字、地址结构的起始位置、这份结构占多少字节。它告诉系统：“这个套接字要使用本机的 127.0.0.1:19090。”

这里首次出现较长的转换：

```cpp
reinterpret_cast<const sockaddr*>(&address)
```

`&address` 是指向 sockaddr_in 的指针；bind 的统一接口接收 `const sockaddr*`，因为它要支持不同地址族。我们把同一块 IPv4 地址内存以这个统一指针类型传进去，系统根据 sin_family 和结构长度识别地址。

这个转换不会重新分配内存、不会把 IP 变成文本、也不会替你转换大小端。内存仍是 address 那一块，所以前一节必须正确填字段。[Linux bind 手册](https://man7.org/linux/man-pages/man2/bind.2.html)说明了地址指针和地址长度的接口约定。

### 6.7 listen：进入监听状态，尚未取出客户端

```cpp
check_call(::listen(listener.get(), 1), "开始监听失败");
std::cout << "正在监听 127.0.0.1:19090，等待一次图片请求……"
          << std::endl;
```

listen 让这个套接字成为等待连接的监听资源。成功后输出提示，`std::endl` 在换行的同时刷新输出流，便于你立即看到服务端已经到这里。

第二个参数 1 是 backlog，涉及等待被 accept 取出的连接队列上限；操作系统也可能调整它。它不是“只允许处理一张图”、不是线程数，也不是一个客户端可以发送的字节数。[Linux listen 手册](https://man7.org/linux/man-pages/man2/listen.2.html)说明了 backlog 的意义。

本课只处理一次连接的原因，在下一段代码：只有一次成功的 accept，处理完后 main 返回，没有循环接受下一个客户端。

### 6.8 accept：等待连接，并拿到用于收发的新资源

```cpp
int accepted;
do {
    accepted = ::accept(listener.get(), nullptr, nullptr);
} while (accepted < 0 && errno == EINTR);
net::SocketFd connection(accepted);
std::cout << "客户端已连接\n";
```

这三个实参分别是：监听套接字、接收对端地址的位置、对端地址长度的位置。两个 nullptr 表示本课不保存客户端的地址信息；不是“客户端地址为空，不能连接”。

do-while 先执行一次 accept，再检查是否因为 EINTR 失败。如果是中断，就重新等；成功或其他错误则退出这个循环。其他错误得到的 -1 会被下面的 SocketFd 构造函数拒绝，并进入外层 catch。

成功时，accepted 是**新描述符**。例如 listener 是 3，accepted 是 4，后面的收发使用 `connection.get()`，也就是 4。原来的 3 仍代表监听资源。

内核负责维护连接状态和 TCP 的建立过程。我们的 accept 是从等待接受的连接中取出一个；你不需要在这里自己写握手报文。Python 的连接过程返回成功，也不代表 C++ 已经读完图片，两端仍要执行后面的收发。

### 6.9 接收请求：先收头，再决定消息体大小

```cpp
std::array<std::uint8_t, protocol::header_size> header_bytes{};
net::recv_exact(connection.get(), header_bytes.data(),
                header_bytes.size());
const auto header = protocol::decode(header_bytes.data(),
                                      header_bytes.size());
if (header.type != protocol::Type::image) {
    throw std::runtime_error("服务端只接受图片请求");
}
std::vector<std::uint8_t> body(header.payload_length);
net::recv_exact(connection.get(), body.data(), body.size());
```

按行看内存发生的变化：

1. 创建有 24 个元素的数组 header_bytes。此时只是准备内存，尚未收到数据。
2. `.data()` 取得第一个元素的地址，`.size()` 得到 24。recv_exact 把收到的头部字节写进数组，收齐才返回。
3. decode 读数组，检查 VFLW 标记、版本、类型、flags 和长度上限，返回已经解释好的 Header 字段。它不再操作网络。
4. 类型必须为 image。protocol 能识别三种合法类型，但服务端这个入口只接收图片请求。
5. `vector(header.payload_length)` 真正创建这么多个字节元素。例如体长 1000，就建立大小为 1000 的可写缓冲区，不是只 reserve 容量。
6. 第二次 recv_exact 收齐 JPEG 消息体。这次长度来自已经校验的头，不再固定为 24。

为什么不能先建图片 vector？在收完头之前，还不知道对方声明的长度；在校验之前，长度也不能直接信任。现有 decode 默认拒绝空体和超过 16 MiB 的体，所以按合法长度分配内存发生在它之后。

**收齐头，不等于收齐请求。** 只有第二次 recv_exact 也完成，body 里才有本次声明的全部图片字节。若对方中途断开，就不会继续图像处理，而会沿异常进入外层 catch。

### 6.10 图片解码和处理：字节怎样变成算法输入

```cpp
const cv::Mat image = cv::imdecode(body, cv::IMREAD_COLOR);
if (image.empty()) {
    throw std::runtime_error("消息体无法解码为图片");
}
error_code = "processing_failed";
auto processor = create_processor("lane_lines", 3);
const auto result = processor->process(image);
```

imdecode 的输入是装着压缩图片内容的 body，而不是文件路径。`cv::IMREAD_COLOR` 要求按普通彩色图像解码，当前处理器接收 8 位 BGR 图片。OpenCV 解码失败可能返回空 Mat，也可能抛异常，因此这里既检查 empty，也放在 try 中。[OpenCV 图片解码文档](https://docs.opencv.org/4.x/d4/da8/group__imgcodecs.html)说明了内存解码接口。

读取出来的字节数量正确，并不证明它们是有效图片。比如 `configs/vision.json` 也是一段非空字节，但不会因为装进 VFLW 图片请求就自动变成 JPEG。

create_processor 和 process 是已经写好的代码：工厂创建 LaneLinesProcessor，process 计算结果并返回 ProcessResult。这部分不重写算法，也不重新讲多态。网络入口的任务是把收到的字节准备成算法所需的 Mat。

这里固定传入 3，没有读取 CLI 的 `--config`。因此你之前的 `configs/vision.json` 中 max_lines=2 不会影响这个独立服务端。

### 6.11 从 ProcessResult 构造响应 JSON

```cpp
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
```

这一步与前面 JSON 课程中的对象构造相同，只是多了请求编号。已有 result 提供尺寸和统计；lines 先创建空数组，再把每条 `cv::Vec4i` 中的四个坐标追加进去：

```cpp
for (const auto& line : result.lines) {
    response["lines"].push_back(nlohmann::json::array({
        line[0], line[1], line[2], line[3]
    }));
}
```

response 是内存里的 JSON 对象，尚未发送；result 中用于观察的 edges、preview 等 Mat 没有放进响应。这份服务端没有图片文件保存步骤。

请求头中的 request_id 会同时写入响应头和 JSON。响应头便于网络层识别这条消息，JSON 中的编号便于应用拿到结果后继续关联请求。本课固定用 42，重点是**响应沿用收到的编号**，不是服务端自行生成另一个数。

### 6.12 两层 try：什么能回错误 JSON，什么只能结束连接

完整代码里，main 的外层 try 内部还有一层图像处理 try。它们作用范围不同：

```text
main 外层 try
  创建监听 → 接受连接 → 收头 → 校验头 → 收齐体
  定义 response、response_type、error_code
  内层 try
    解码图片 → 调用处理器 → 构造成功 JSON
  内层 catch
    构造错误 JSON，准备发送
  序列化 → 封装响应 → 发送 → return 0
main 外层 catch
  显示服务端失败 → return 1
```

response 等变量定义在内层 try 之前，所以内层 catch 和后续发送都能使用。你在日志课已经理解过作用域，这里在新场景里应用同一原则。

| 失败发生的位置 | 被哪层捕获 | 客户端可能看到什么 |
| --- | --- | --- |
| bind、accept、消息头校验、收不齐请求体 | 外层 catch | 连接建立失败或连接结束，没有本课的业务错误 JSON |
| JPEG 无法解码、处理器失败 | 内层 catch | 已准备好 type=3 的错误响应；发送成功时能收到错误 JSON |
| 响应超限、发送失败 | 外层 catch | 响应未能完整送出，客户端可能报收不齐 |

error_code 最初是 `invalid_image`，图片解码成功后改为 `processing_failed`。这样内层失败时，可以区分“输入图片不能解码”和“后续处理阶段失败”。catch 把 response_type 从 result 改成 error，把 status 改成 error，再记录中文错误说明。

坏消息头可能连长度或请求编号都不可信，所以本课直接结束连接；只有校验头、收齐体之后的业务处理失败，才按可信的请求信息构造错误响应。返回错误 JSON 也可能因断连而发送失败，内层 catch 不保证网络一定成功。

### 6.13 把 JSON 变成字节、加头、发送和退出

```cpp
const std::string json_text = response.dump();
constexpr std::size_t max_response = 64U * 1024U;
if (json_text.size() > max_response - protocol::header_size) {
    throw std::runtime_error("响应内容超过允许上限");
}
const std::vector<std::uint8_t> response_body(json_text.begin(),
                                                json_text.end());
const auto message = protocol::packet(response_type,
                                       header.request_id,
                                       response_body);
net::send_all(connection.get(), message.data(), message.size());
```

按类型变化理解：

| 变量 | 内存中装着什么 | 从哪里来 |
| --- | --- | --- |
| response | JSON 对象 | 处理结果，或错误信息 |
| json_text | 序列化后的 JSON 文本，std::string | response.dump() |
| response_body | JSON 文本的字节 vector | 用字符串的 begin、end 构造 |
| message | 24 字节头 + JSON 体 | protocol::packet() |

字符串的 begin 和 end 表示要复制的字符范围，end 指向末尾之后；vector 构造把这个范围内的字节复制进去。这里 std::string 的 size 是字节数，包括 UTF-8 中文占用的全部字节，不是显示出来的汉字个数。

响应总长上限为 65536 字节，已经包含头。因此体长最多是 `65536 - 24 = 65512`。请求图片体的 16 MiB 上限和响应的 64 KiB 总长上限是两个方向的限制，别把它们当成同一个数字。

假设 JSON 文本为 200 字节，packet 就生成 224 字节的 message，头中 payload_length 写 200，send_all 的 size 则传 224。这再次说明“体长”和“整条消息长度”用途不同。

发送函数正常返回后，打印完成提示并 return 0，然后局部对象析构、连接和监听描述符被关闭。服务端不会再去 accept 第二个客户端。

这里 return 0 表示程序成功完成了本次响应的发送流程；响应也可能是 type=error。图像业务是否成功由响应类型和 status 判断，客户端收到错误响应时会返回 1。成功发送仍不等于客户端应用一定已经解析完成。

## 7. Python 客户端：python/vision_client.py

### 7.1 文件的完整内容

打开 `/home/rikanshin/VisionFlow/VisionFlow_learn/python/vision_client.py`。客户端仅使用 Python 标准库，不需要另装网络库或 OpenCV。

```python
import json
from pathlib import Path
import socket
import struct
import sys


HEADER = struct.Struct("!4sHHQII")
MAX_PAYLOAD = 16 * 1024 * 1024
MAX_RESPONSE_BODY = 64 * 1024 - HEADER.size


def recv_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise ConnectionError("连接已关闭，尚未收齐消息数据")
        data.extend(chunk)
    return bytes(data)


def main():
    input_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("samples/frame.jpg")
    if len(sys.argv) > 2:
        raise ValueError("用法：python3 python/vision_client.py [图片路径]")
    jpeg = input_path.read_bytes()
    if not 0 < len(jpeg) <= MAX_PAYLOAD:
        raise ValueError("图片文件大小必须在 1 字节到 16 MiB 之间")

    request_id = 42
    request = HEADER.pack(b"VFLW", 1, 1, request_id, len(jpeg), 0) + jpeg
    with socket.create_connection(("127.0.0.1", 19090)) as connection:
        connection.sendall(request)
        print(f"图片请求已发送，请求编号：{request_id}")
        header = recv_exact(connection, HEADER.size)
        magic, version, kind, response_id, length, flags = HEADER.unpack(header)
        if magic != b"VFLW" or version != 1 or flags != 0:
            raise ValueError("响应消息头格式不正确")
        if kind not in (2, 3):
            raise ValueError("响应消息类型必须是结果或错误")
        if response_id != request_id:
            raise ValueError("响应编号与请求编号不一致")
        if not 0 < length <= MAX_RESPONSE_BODY:
            raise ValueError("响应消息体长度超出允许范围")
        body = recv_exact(connection, length)
        result = json.loads(body.decode("utf-8"))
        if not isinstance(result, dict) or result.get("request_id") != request_id:
            raise ValueError("JSON 结果与请求编号不一致")
        expected_status = "ok" if kind == 2 else "error"
        if result.get("status") != expected_status:
            raise ValueError("响应类型与 JSON 状态不一致")
        print("收到处理结果：" if kind == 2 else "收到错误响应：")
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if kind == 2 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, struct.error) as error:
        print(f"客户端失败：{error}", file=sys.stderr)
        sys.exit(1)
```

这个文件有三层：开头的协议常量，负责收齐字节的 recv_exact，以及负责一次请求响应的 main。文件最下面的入口调用 main，并把成功或失败转换成进程退出码。

### 7.2 Struct 的格式字符串，怎样对应 C++ 的头

```python
HEADER = struct.Struct("!4sHHQII")
```

Struct 对象保存一份打包／解包规则，名字 HEADER 是本课给它起的变量名。它还不是某次请求的头部字节；调用 pack 才会生成实际字节。

| 格式片段 | 本课对应字段 | 占用字节 | 传给 pack 的值 |
| --- | --- | --- | --- |
| `!` | 整份格式采用网络字节序、标准大小、无自动填充 | 不占字段字节 | 不单独传值 |
| `4s` | magic | 4 | b"VFLW" |
| 第一个 `H` | version，无符号 16 位整数 | 2 | 1 |
| 第二个 `H` | type，无符号 16 位整数 | 2 | 1，表示 image |
| `Q` | request_id，无符号 64 位整数 | 8 | 42 |
| 第一个 `I` | payload_length，无符号 32 位整数 | 4 | len(jpeg) |
| 第二个 `I` | flags，无符号 32 位整数 | 4 | 0 |

所以 `HEADER.size` 等于 `4 + 2 + 2 + 8 + 4 + 4 = 24`。`4s` 的 4 是这个字节串字段的宽度；不是四个整数，也不是包括字符串结束符在内的长度。`!` 已经规定大端，这里不用再给整数额外调用 htons。

Python 的 pack 与 C++ 前一课的 encode 实现方式不同，但产出的字段顺序、宽度、字节序相同，因此可以互相读懂。[Python struct 文档](https://docs.python.org/3/library/struct.html)给出这些格式符及大小的定义。

### 7.3 路径怎样变成要发送的字节

```python
input_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("samples/frame.jpg")
if len(sys.argv) > 2:
    raise ValueError("用法：python3 python/vision_client.py [图片路径]")
jpeg = input_path.read_bytes()
```

调用 `python3 python/vision_client.py samples/frame.jpg` 时，sys.argv[0] 是脚本名称，sys.argv[1] 是图片路径。第一行是条件表达式：提供路径就用它，否则使用默认路径；如果额外传入多个参数就报错。

`Path(...)` 创建路径对象，尚未读文件；`read_bytes()` 才打开文件、读出二进制内容并关闭文件，返回 bytes。相对路径仍以启动程序时的当前目录为基准，所以第 9 节要求进入学习工程根目录。[Path.read_bytes 文档](https://docs.python.org/3/library/pathlib.html#pathlib.Path.read_bytes)说明了返回类型。

此时 jpeg 的内容是 JPEG 文件字节，变量名本身不会检查文件格式。接下来的范围检查只判断文件非空且不超过 16 MiB；真正是否能解码为图片，由服务端的 OpenCV 判断。

```python
if not 0 < len(jpeg) <= MAX_PAYLOAD:
    raise ValueError("图片文件大小必须在 1 字节到 16 MiB 之间")
```

Python 支持连续比较，`0 < len(jpeg) <= MAX_PAYLOAD` 表示两个条件都成立。外面的 not 表示不满足时进入报错分支。len(jpeg) 数的是字节数，不是图片宽、高或像素数。

### 7.4 pack 和加号，具体生成了什么

```python
request_id = 42
request = HEADER.pack(b"VFLW", 1, 1, request_id, len(jpeg), 0) + jpeg
```

`b"VFLW"` 是 bytes，包含四个 ASCII 字节；普通的 `"VFLW"` 是 Python 文本字符串。这个 `4s` 字段需要字节，因此这里有 b 前缀。

先执行 pack，得到 24 字节的 bytes；再通过 `+ jpeg` 拼接图片体，得到整条请求。假设 jpeg 有 1000 字节，request 就有 1024 字节，头里的 payload_length 仍是 1000。

```text
request 的下标：0 ............ 23 | 24 ...................... 1023
内容：          VFLW 消息头       | 原样读取的 1000 个图片字节
```

这个加号只是在内存中拼接内容，没有发送数据，也没有再次压缩 JPEG。固定编号 42 是本课“一次连接发一次请求”的简化；服务端会原样带回它，客户端后面再核对。

### 7.5 create_connection 和 with：建立连接并管理关闭

```python
with socket.create_connection(("127.0.0.1", 19090)) as connection:
    connection.sendall(request)
```

传入的是二元组 `(地址, 端口)`。create_connection 创建 socket 并尝试连接目标，成功后返回已经连接好的 socket 对象；`as connection` 为这个对象命名。服务端必须先启动并监听对应地址。

为什么客户端这里没有单独写 bind？普通主动连接可以由操作系统选择客户端本地地址和可用端口。它连接的目标端口是服务端的 19090，客户端自己的本地端口通常由系统选择，不必也设成 19090。

with 管理对象的使用期限：缩进块正常结束、return 或因异常离开时，会关闭连接。它与 C++ SocketFd 的用途相似，让资源释放跟随作用域。[Python socket 文档](https://docs.python.org/3/library/socket.html#socket.create_connection)说明了连接辅助函数及 socket 的上下文管理支持。

sendall 会持续尝试把传入内容发送完，失败时抛异常；因此不必在 Python 里再抄一份 C++ send_all。正常返回表示本次发送操作完成，不保证 C++ 已处理完图片，所以后面还要等待响应。

### 7.6 Python 的 recv_exact 怎样拼接多次收到的内容

```python
def recv_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise ConnectionError("连接已关闭，尚未收齐消息数据")
        data.extend(chunk)
    return bytes(data)
```

这次系统接口的使用形式和 C++ 不同：Python 的 recv 返回收到的 bytes，我们自己把它追加到 data；C++ 的 recv 则把字节写入传入的内存，返回字节数量。

| 变量或操作 | 在这段函数中的作用 |
| --- | --- |
| bytearray() | 创建可增长、可修改的字节容器，开始时为空 |
| len(data) | 已经收到多少，相当于 C++ 的 done |
| size - len(data) | 本次最多还接收多少 |
| chunk | 这次新收到的 bytes |
| data.extend(chunk) | 把 chunk 中的所有字节追加到末尾 |
| bytes(data) | 收齐后转换成 bytes 返回给调用者 |

例如总共需要 24 字节，第一轮收到 10 字节，extend 后 len(data)=10；第二轮只向 recv 请求剩余的 14 字节。若这次只返回 8，容器增至 18，下一轮再要 6。这个过程与第 5 节相同，但 Python 替我们管理容器扩容和内存位置。

bytes 不可直接修改，bytearray 适合逐次追加；extend 追加一段字节，避免把 chunk 当成一个单独元素。`if not chunk` 判断是否收到空 bytes，即对端结束发送；当前 size 为正且尚未收齐，因此抛异常。

接收仍需循环，sendall 并没有自动提供一个与之对应的“收齐整条 VFLW 消息”的功能。[Python Socket 编程指南](https://docs.python.org/3/howto/sockets.html)也强调需要按应用格式处理接收长度。

### 7.7 unpack：把响应头恢复成六个值，并逐项检查

```python
header = recv_exact(connection, HEADER.size)
magic, version, kind, response_id, length, flags = HEADER.unpack(header)
```

第一行得到完整 24 字节。第二行按相同格式解包，返回六个值，再依次赋给左侧六个变量。比如 kind 是 Python 整数 2，length 是响应体的字节数；它们已经不是大端字节数组，而是可以直接比较和计算的值。

紧接着的检查分别保护不同约定：

| 检查内容 | 为什么要检查 |
| --- | --- |
| magic、version、flags | 确认这段头遵循当前 VFLW 版本 |
| kind 是否为 2 或 3 | 客户端此时只接受结果或错误响应 |
| response_id 是否等于 request_id | 确认响应属于本次请求 |
| length 是否在 1～65512 | 在继续接收前限制响应体大小 |

所有检查通过后，才执行 `body = recv_exact(connection, length)`。这和 C++ 服务端的“收头 → 校验 → 收体”完全对应，只是两端接受的消息类型及长度上限不同。

### 7.8 bytes → 文本 → 字典：为什么是两次转换

```python
result = json.loads(body.decode("utf-8"))
```

从括号里面往外读：

```text
body                         bytes：网络收到的 JSON 字节
    ↓ decode("utf-8")
JSON 文本                    str：可按文本解释的内容
    ↓ json.loads(...)
result                       dict：可通过键取值的结果对象
```

UTF-8 解码负责把字节解释为文本；JSON 解析负责把文本中的对象、数组、数字等恢复成 Python 数据。两者处理的是不同层次。JPEG 体不走这个过程；只有响应体约定为 JSON 时才这样解析。

JSON 对象在 Python 中变成 dict，JSON 数组变成 list。例如 `result["width"]` 是整数，`result["lines"]` 是线段列表。这里复用你已经学过的 JSON 概念。[Python json 文档](https://docs.python.org/3/library/json.html)列出了对应类型。

### 7.9 为什么收到 JSON 后还要检查编号和状态

```python
if not isinstance(result, dict) or result.get("request_id") != request_id:
    raise ValueError("JSON 结果与请求编号不一致")
expected_status = "ok" if kind == 2 else "error"
if result.get("status") != expected_status:
    raise ValueError("响应类型与 JSON 状态不一致")
```

JSON 解析成功只证明文本语法合法。合法 JSON 也可能是一个数字或数组，因此先检查结果是 dict。or 会短路：如果不是 dict，后面的 get 不会执行；如果是 dict，就检查它携带的请求编号。get 在字段不存在时返回 None，这里也会导致不匹配。

头部 type=2 应对应 status="ok"；type=3 应对应 status="error"。expected_status 把这个约定写成条件表达式，再与实际值核对。这是本课的基础一致性检查，并没有全面校验所有坐标和统计字段的类型。

### 7.10 显示结果、退出码与文件入口

```python
print("收到处理结果：" if kind == 2 else "收到错误响应：")
print(json.dumps(result, ensure_ascii=False, indent=2))
return 0 if kind == 2 else 1
```

json.dumps 把字典重新排版为方便阅读的 JSON 文本。indent=2 表示每层缩进两空格；ensure_ascii=False 使中文直接显示。这里是终端展示，不会把重新排版的文本再发给服务端。

收到成功结果返回 0；收到合法错误响应返回 1。这个 return 先离开 with，连接被关闭，然后 main 返回对应整数。

```python
if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, struct.error) as error:
        print(f"客户端失败：{error}", file=sys.stderr)
        sys.exit(1)
```

直接运行此脚本时，入口条件成立。main 的返回值交给 sys.exit，成为终端可以读取的退出码。若读文件、连接、接收、解析或校验抛异常，就显示中文失败提示，并以 1 退出。

OSError 覆盖文件与网络错误，ConnectionError 也是它的子类；ValueError 覆盖这里主动报告的值错误，也覆盖 JSON 解码错误等相关子类；struct.error 处理打包格式错误。系统或库附带的原始错误详情可能是英文，保留它方便定位原因。

## 8. 把两端代码连起来，跟踪一条请求

现在用一组假定长度完整走一遍：JPEG 是 1000 字节，结果 JSON 是 200 字节，请求编号 42。下面的长度仅用于演算，不是 samples/frame.jpg 的实测大小。

| 顺序 | 哪一端、哪段代码 | 这一步完成之后 |
| --- | --- | --- |
| 1 | 服务端 socket → bind → listen | 127.0.0.1:19090 已处于监听状态 |
| 2 | 服务端 accept | 没有连接时在此等待 |
| 3 | 客户端 read_bytes → HEADER.pack | 内存里准备好 1024 字节请求，头中体长为 1000 |
| 4 | 客户端 create_connection，服务端 accept 成功 | 两端各持有本次连接的 socket，服务端还有独立的 listener |
| 5 | 客户端 sendall(request) | 把 1024 字节请求交给系统发送 |
| 6 | 服务端 recv_exact(..., 24) → decode | 收齐头，得到 type=image、编号 42、体长 1000 |
| 7 | 服务端 recv_exact(..., 1000) | body 里有完整的图片字节 |
| 8 | 服务端 imdecode → process | 得到像素矩阵，再得到 ProcessResult |
| 9 | 服务端构造 JSON → dump → packet | 得到 224 字节响应，头中编号 42、体长 200 |
| 10 | 服务端 send_all | 发送响应，然后结束本次服务 |
| 11 | 客户端 recv_exact(..., 24) → unpack | 得到响应类型、编号、体长 200，并校验 |
| 12 | 客户端 recv_exact(..., 200) → decode → loads | 得到 Python 结果对象，检查后显示 |

表格用于表达依赖顺序；实际两端会交错执行。客户端可能已经在等响应，而服务端还在处理图片。每个方向的字节顺序由 TCP 保持，应用继续用头部长度确定一条消息的边界。

本课客户端先发请求再等响应，服务端先收请求再发响应，双方的安排能配合起来。如果两边都先等对方发数据，就可能互相等待。

还要分清两种“输出”：`std::cout` 和 Python print 写到各自终端；send_all 和 sendall 写到连接。服务端显示“客户端已连接”不会自动变成客户端收到的 JSON，真正回传的数据来自封装好的 message。

## 9. CMake 配套与由你执行的运行观察

### 9.1 CMake 这段代码为什么要包含这些文件

学习工程 `/home/rikanshin/VisionFlow/VisionFlow_learn/CMakeLists.txt` 末尾已经有下面的配套目标，**不需要重复添加**：

```cmake
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/vision_server.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/tcp_io.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/tcp_io.hpp")
    add_executable(vision_server
        apps/vision_server.cpp
        src/tcp_io.cpp
        src/protocol.cpp
        src/processor.cpp
        src/line_utils.cpp
    )
    target_include_directories(vision_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(vision_server PRIVATE
        ${OpenCV_LIBS}
        nlohmann_json::nlohmann_json
    )
endif()
```

三个 EXISTS 检查针对服务端入口、网络实现和网络头文件。以前文件未写齐时跳过这个目标；本次只读核对发现文件已经存在，后续由你重新配置 CMake 即可让它按当前文件状态生成目标。

| 配置中的内容 | 为什么需要 |
| --- | --- |
| apps/vision_server.cpp | 提供服务端的 main |
| src/tcp_io.cpp | 提供 SocketFd、recv_exact、send_all 的定义 |
| src/protocol.cpp | 提供 decode 和 packet 等协议实现 |
| src/processor.cpp、src/line_utils.cpp | 提供图像处理和线段辅助函数 |
| include 目录 | 让编译器能找到三个项目头文件 |
| OpenCV | 解码和处理图片 |
| nlohmann_json | 构造和序列化 JSON |

没有把 apps/vision_cli.cpp 加进来，因为它有自己的 main；vision_cli 与 vision_server 是两个可执行目标。Python 文件也不加入 C++ 编译列表，它由 Python 解释器执行。

### 9.2 两个终端的启动顺序

下面是你准备好后使用的命令，老师未执行这些课程操作。单纯阅读这次改写不要求重新运行。

在两个终端中，都进入同一个 WSL 发行版的学习工程目录：

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
```

先在终端一配置、构建并启动服务端：

```bash
cmake -S . -B build
cmake --build build --target vision_server
./build/vision_server
```

服务端预期先显示：

```text
正在监听 127.0.0.1:19090，等待一次图片请求……
```

此时终端还没有回到命令提示符，通常是正在 accept 等客户端。让它保持运行，再在终端二执行：

```bash
python3 python/vision_client.py samples/frame.jpg
echo $?
```

客户端正常情况下先显示“图片请求已发送，请求编号：42”，随后显示“收到处理结果：”和 JSON。服务端会继续显示“客户端已连接”“响应已发送，本次连接处理结束”，然后退出。终端一回到提示符后，也可以用 `echo $?` 查看服务端退出码。

echo $? 应紧接在要观察的程序退出后执行；若中间执行了其他命令，看到的是其他命令的退出状态。客户端与服务端退出码分别在各自终端查看。

### 9.3 预期结果该怎样判断

| 观察项 | 根据当前代码推导的正常情况 |
| --- | --- |
| 客户端退出码 | 0 |
| 服务端退出码 | 0 |
| JSON 的 request_id | 42，与本次请求一致 |
| JSON 的 status | ok |
| JSON 的 processor | lane_lines |
| 使用当前样例图片时的 width、height | 640、360 |
| lines | 数组，最多 3 条线段 |

这些是根据代码和已有样例推导的预期，不能当成老师运行过的记录。服务端这次不保存图片或 build/result.json；结果从网络回到客户端终端，已有 CLI 的输出文件是否存在与这次网络运行无关。

一次成功往返后服务端已经退出。第二次运行客户端前，需要再次启动 `./build/vision_server`；无需为了每次连接都重新编译。

### 9.4 可选：观察业务错误仍然可以形成合法响应

如果你想观察两层异常处理，在重新启动服务端后，让客户端发送这个非图片文件：

```bash
python3 python/vision_client.py configs/vision.json
echo $?
```

这个文件非空，客户端按当前代码允许把它封装为 image 请求；服务端收头、校验长度、收体都能走到下一步，但图片解码会失败。预期客户端收到 type=3、status="error"、code="invalid_image" 的错误 JSON，并返回 1；服务端若成功发送这个响应，则返回 0。

这说明“收到合法响应”和“图像处理成功”是两件事。这个观察是可选项，不作为继续学习的额外强制门槛。

## 10. 从输出和报错，判断代码执行到哪里

| 看到的现象 | 对照的代码位置 | 优先检查什么 |
| --- | --- | --- |
| unknown target vision_server | CMake 还没有生成这个目标 | 三个 C++ 文件是否在指定位置，是否重新执行配置 |
| 客户端提示找不到图片文件 | Path.read_bytes | 当前工作目录和传入路径；此时还没开始连接 |
| Connection refused／连接被拒绝 | create_connection | 服务端是否仍在监听同一 WSL 的 19090；上一次是否已处理完退出 |
| 绑定监听地址失败，详情为 Address already in use | bind | 是否还有另一个服务端正在使用这个地址和端口 |
| 只显示“正在监听……” | accept 之前的提示已完成 | 尚未接受客户端时，等待是预期行为 |
| 显示“客户端已连接”，但还没有响应 | accept 已完成，后续在接收或处理 | 客户端是否确实发送；声明的体长和实际内容是否一致；服务端是否输出了错误 |
| “连接已关闭，尚未收齐消息数据” | recv_exact | 对端在所需字节收齐前结束了发送；结合另一端错误定位原因 |
| 收到错误响应，code 为 invalid_image | 服务端内层图片解码阶段 | 输入文件内容能否被解码为图片 |
| 请求编号不一致／状态不一致 | 客户端响应校验 | 响应头、JSON、请求之间是否遵守约定 |

不能仅凭“等待”判断程序卡死。例如 accept 等客户端很正常；但对方声明体长 1000、只发送 900，又一直保持连接，接收端就可能在等剩余 100 字节。本课没有超时机制，所以要结合两端执行位置判断。

正常退出时 SocketFd 和 with 负责关闭连接。你不需要为了让 recv_exact 识别消息结束而提前 close：消息边界已经由长度字段表达；提前关闭可能使另一端收不齐数据或无法回传。

## 11. 本知识点的集中理解检查

读完并结合自己的代码后，集中说明下面三点即可，不需要每小节做一次额外实验：

1. listener 和 connection 分别由哪次系统调用产生？为什么收发使用 connection？
2. 需要收齐 24 字节，第一轮 recv 返回 10。下一轮的写入位置和最大接收长度分别是什么？若仍从原地址写，会发生什么？
3. 从客户端的 JPEG 文件到服务端的 Mat，再到客户端的结果字典，中间经过哪些转换？哪些函数真正执行网络收发？

本课把 VFLW 编解码接入一次真实连接。主要代码和课程运行仍由你完成；老师依据你提供的输出与解释继续分析。一次本机成功往返能证明基本链路工作，不能说明部分收发、断连和信号中断分支都实际发生过；没有展示的情况不记作已经验证。
