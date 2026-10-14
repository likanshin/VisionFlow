# 第八模块问答总结：noexcept、epoll 与连接状态机

> 整理时间：2026-10-14
> 范围：模块 8（非阻塞 TCP / epoll 服务端）开发过程中的提问与解答，覆盖 `SocketFd` 移动语义、epoll 原理、构建命令、非阻塞收发状态机、超时与调度。
> 配套讲义：[08-1-nonblocking-epoll.md](08-1-nonblocking-epoll.md)
> 主要代码：[apps/vision_epoll_server.cpp](../../apps/vision_epoll_server.cpp)、[include/tcp_io.hpp](../../include/tcp_io.hpp)、[src/tcp_io.cpp](../../src/tcp_io.cpp)

---

## Q1 移动构造函数为什么要加 `noexcept`

**结论**：`noexcept` 是一份契约——"我绝不抛异常，抛了就直接 `std::terminate`"；对移动构造而言，它还是给标准库看的**选路信号**，决定容器搬家时走移动还是退回拷贝。

**要点**

1. **契约语义**：不是优化提示。违反了不会栈展开，不会走 `catch`，程序直接终止。所以只有确定不会抛（比如只做 `int` 赋值）才敢标。
2. **必须明写**：声明在头文件、定义在 `.cpp` 的函数，调用点看不到函数体，编译器默认认为"可能抛"。对于分离编译的移动构造，不标就是"可能抛"。
3. **标准库怎么读它**——`std::vector` 扩容时要决定搬运方式（内部即 `std::move_if_noexcept` 的逻辑）：

   | 移动构造 | 类型可拷贝？ | vector 的选择 |
   |---|---|---|
   | `noexcept` | 均可 | 用移动（快） |
   | 可能抛 | 可拷贝 | **退回拷贝**（慢，移动构造白写） |
   | 可能抛 | 不可拷贝 | 只能移动，但异常时"效果未指定" |

4. 同样的机制还影响 `std::swap`、排序、`std::optional`/`std::variant`/`std::pair` 的 noexcept 传播。
5. `noexcept` 是类型系统的一部分：`std::is_nothrow_move_constructible_v<SocketFd>` 可在编译期查询、断言。

**实现**（`std::exchange` 一句完成"取值 + 清空"，来自 `<utility>`）：

```cpp
SocketFd::SocketFd(SocketFd&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}
```

**顺带**：类里有用户声明的析构函数和移动构造时，隐式移动赋值不会生成；拷贝赋值又被 `= delete`，所以 `a = std::move(b);` 编译不过。需要的话要显式声明 `SocketFd& operator=(SocketFd&&) noexcept;`。

---

## Q2 移动后源对象 fd 变成 -1，析构不再 close，那 fd 什么时候销毁？

**结论**：移动是**所有权转移**，不是复制。整个生命周期里这个 fd 只会被 `close` 一次——由当时的持有者（也就是新主人）的析构函数执行。

**要点**

1. **两层"销毁"要分清**：

   | | 归谁管 | 何时发生 |
   |---|---|---|
   | `SocketFd` 这个 C++ 对象 | 编译器 | 离开作用域自动析构 |
   | 内核里的 socket（fd） | **当时持有它的对象** | 由新主人的析构函数 `::close` |

2. 源对象变成空壳（`fd_ == -1`），它的析构**什么都不做——这是正确的**，不是遗漏：它已经不拥有任何资源。
3. **时间线**：

   ```cpp
   {
       SocketFd a(::socket(AF_INET, SOCK_STREAM, 0));  // a.fd_ = 7
       SocketFd b(std::move(a));                       // b.fd_ = 7，a.fd_ = -1
   }   // 逆序析构：b 先 → close(7)（全程序唯一一次）；a 后 → fd_ == -1，跳过
   ```

4. **不置 -1 的后果：double close**。两个对象都持有 7，先析构的把 7 归还内核，这个号码可能立刻被下次 `socket()`/`accept()` 复用给别人；后析构的再 `close(7)` 关掉的就是别人的 fd。现象是"连接莫名断开"，极难排查。
5. 这就是 `unique_ptr` 的套路：**move-only 的 RAII 句柄**，移动后源对象处于明确、合法、可析构的空状态（valid but unspecified state 的具体化）。标准库的 `std::thread`、`std::fstream`、`std::unique_lock` 同理。
6. **析构里的守卫**：

   ```cpp
   SocketFd::~SocketFd() {
       if (fd_ >= 0) { ::close(fd_); }
   }
   ```

   价值不在省一次系统调用（`close(-1)` 无害），而在于**显式承认 `-1` 是合法空状态**，并与构造函数的不变量（要么有效 fd 要么抛异常）对上。空值必须用 `-1` 而非 `0`，因为 `0` 是合法 fd（stdin）。
7. **析构函数绝不能抛异常**。`::close` 可能失败（EINTR 等），静默忽略即可。

---

## Q3 epoll 是什么

**结论**：Linux 的 **I/O 多路复用**——用一个线程同时监视大量 fd，内核只把"已就绪"的那些报告给你。

**要点**

1. **不是异步 I/O**。epoll 只通知"可以读了"，读还得自己调 `recv`。真正的异步 I/O 是 `io_uring`。
2. **动机**：
   - 一连接一线程：线程数爆炸、大部分线程在阻塞睡大觉；
   - 非阻塞轮询：CPU 空转；
   - `select`/`poll`：每次调用全量拷贝 + 线性扫描，O(n)，且 fd 越多越慢；`select` 还有 1024 上限。
3. **epoll 的核心设计：把"注册关注"和"等待事件"分开**
   - 内核里维护持久对象（本身也是个 fd）：红黑树存注册的 fd，就绪链表存已就绪的 fd；
   - `epoll_ctl` 注册一次；数据到达时内核把 fd 挂进就绪链表；
   - `epoll_wait` 只返回就绪链表 → **O(就绪数量)**，与总连接数无关。
4. **三个 API**：

   ```cpp
   SocketFd epfd(::epoll_create1(0));                          // 创建实例
   ::epoll_ctl(epfd.get(), EPOLL_CTL_ADD, fd, &ev);            // 注册/修改/删除
   ::epoll_wait(epfd.get(), events, n, timeout);               // 等待（-1 无限 / 0 立即 / >0 毫秒）
   ```

5. **LT vs ET**：

   | | 水平触发 LT（默认） | 边缘触发 ET（`EPOLLET`） |
   |---|---|---|
   | 通知时机 | 只要还有数据，每次都通知 | 只在"从无到有"那一刻通知一次 |
   | 要求 | 无 | fd 非阻塞 + 必须循环读到 `EAGAIN` |
   | 漏读 | 下次还会提醒 | **永不通知，连接卡死** |

   本项目用的是 LT。
6. **EPOLLOUT 是可写常态**：发送缓冲区大部分时间是空的，一直订阅会变成忙等。正确做法是"按需订阅"：平时只关心 `EPOLLIN`，有数据要发才 `EPOLL_CTL_MOD` 加 `EPOLLOUT`。`EPOLLERR`/`EPOLLHUP` 不用注册也会报。
7. **适用边界**：适合大量连接、每个连接不总活跃；不适合只有几个连接，也不适合普通磁盘文件（文件永远"就绪"）。epoll 是 Linux 专有，跨平台对应 `kqueue`（BSD/macOS）、`IOCP`（Windows）。

---

## Q4 `cmake --build build --target vision_epoll_server -j2` 是什么意思

**结论**：在 `build/` 里只构建 `vision_epoll_server` 这一个目标，最多并行 2 个编译任务。本机生成器是 Ninja，实际执行 `ninja -j 2 vision_epoll_server`。

**要点**

1. **CMake 分两个阶段**：

   | 阶段 | 命令 | 做什么 |
   |---|---|---|
   | ① 配置生成 | `cmake -B build` | 读 CMakeLists.txt，探测依赖，把构建脚本写进 `build/` |
   | ② 构建 | `cmake --build build` | CMake 当"传话人"，去 build/ 里调用真正的构建工具（Ninja） |

2. `--build build`：`build` 是构建目录，必须是之前配置过的那个。
3. `--target vision_epoll_server`：只构建这一个目标（`add_executable` 起的名字，见 CMakeLists.txt），不构建 `all` 里的其他目标。**只跳过别的目标，不跳过依赖**——源码改了照样重编。
4. `-j2`：并行编译任务数。C++ 每个 `.cpp` 独立编译，可并行。本机 6 核，`-j2` 反而偏保守；**别写裸 `-j`**（无限并行，内存被打爆）。
5. 已经是增量构建：Ninja 比对时间戳，只重编改动过的文件。
6. **坑**：CMakeLists.txt 里 `if(EXISTS ...)` 在工作**配置阶段**求值。如果源文件是配置之后才新建的，`build/` 里根本没有该目标，必须先 `cmake -B build` 重新配置。
7. 等价写法：`cd build && ninja vision_epoll_server -j2`。

---

## Q5 `::socket` 这种 `::` 形式是什么意思

**结论**：`::` 是**作用域解析运算符**，读作"在……里面的"。左边为空表示**全局作用域**，所以 `::socket` 就是"全局命名空间里那个 `socket`"——POSIX 系统调用。

**要点**

1. 四种形态对照：

   | 写法 | 左边是什么 | 含义 |
   |---|---|---|
   | `visionflow::net::SocketFd` | 命名空间 | 那个命名空间里的名字 |
   | `SocketFd::~SocketFd` | 类 | 类作用域里的成员 |
   | `protocol::Type::image` | 嵌套作用域 | 逐层往里 |
   | `::socket` | **空** | **全局**作用域里的 `socket` |

2. **为什么值得写**：无限定名字的查找是**从内层往外，找到第一个就停**。
   - 类里有同名成员 → 优先选中。经典坑：析构函数里写 `close(fd_)` 而类里恰好有成员 `close()`，调用的就是自己；
   - 命名空间同名函数、`using namespace std;`、ADL 都会带来类似风险；
   - `::` 直接锁定全局，关掉这一切。
3. **路径类比**：`visionflow::net::` ≈ 相对路径；`::socket` ≈ 以 `/` 开头的绝对路径。
4. **项目约定**：加了 `::` 的一律是操作系统 API（`::socket`、`::setsockopt`、`::bind`、`::close`），没加的是自家代码（`net::SocketFd`）。在全局作用域（如 `main`）里这层限定是冗余的，但这是**故意的标注**，值得保持。

---

## Q6 非阻塞 `recv` 返回 -1 / `EAGAIN` 怎么处理？与返回 0 有什么区别

**结论**：`EAGAIN` = "**暂时**没有，连接正常，以后可能还有"；`0` = "对端有序关闭（EOF），**永远**没有下一句了"。

**电话类比**：EAGAIN = 拿起听筒对方还没开口，先挂上待会儿再听，**通话还在**；0 = 对方说"我说完了"然后挂了电话，**通话结束**。

**要点**

1. **EAGAIN 的处理：什么都不做，直接 `return`**。
   - 不要抛异常、不要打日志、不要 while 重试（busy loop）、不要关连接；
   - 数据到了 `epoll_wait` 自然会再叫你——把"等"交还给内核；
   - 在 ET 下，读到 EAGAIN 恰好是"本次事件已榨干"的正常出口。
2. **EINTR 不是 EAGAIN**：语义是"被信号打断，重试"。LT 下直接 return 安全（下次还会通知）；**ET 下必须重试**，否则可能永久丢掉剩余数据。
3. **返回 0 的处理取决于状态机阶段**：
   - 消息已收齐 / 响应已发完 → 正常结束（TCP 支持半关闭：对端关了写方向，你仍可发响应）；
   - 消息没收齐 → 对端提前跑了，抛错关连接；
   - 0 之后不要再读，会一直返回 0。
4. **`EPOLLRDHUP` ≠ 立刻关连接**：它只是"对端关闭了写方向"的预告，缓冲区里可能还有没读完的数据。必须继续 `recv` 直到返回 0 才算读干净。
5. **速查表**：

   | 返回 | errno | 含义 | 处理 |
   |---|---|---|---|
   | `> 0` | — | 收到这么多字节（**短读是常态**） | 更新进度，继续 |
   | `-1` | `EAGAIN`/`EWOULDBLOCK` | 暂时没数据，连接正常 | `return`，等下次事件 |
   | `-1` | `EINTR` | 被信号打断 | 重试（LT 下 return 也安全） |
   | `-1` | `ECONNRESET` | 对端 RST | 关连接 |
   | `-1` | 其他 | 真错误 | 关连接、记日志 |
   | `0` | — | 对端有序关闭（EOF） | 看状态机：收齐=正常结束；没收齐=对端提前跑 |

---

## Q7 收到 10 字节后 `read_step` 返回，哪份对象、哪些字段保存状态？下次从哪继续写

**结论**：数据躺在 **`Connection` 对象**的缓冲区里，对象住在 `main()` 的 **`connections` 表**里，靠 **`token`** 找回；进度是 **`header_done` / `body_done`** 偏移量；续写位置 `buffer.data() + done` **每次调用现算**。

**要点**

1. **对象住哪儿**：`Connections connections;` 声明在 `while` 事件循环**外面**，生命周期覆盖整个程序——这是状态能跨 `epoll_wait` 存活的前提。
2. **和阻塞版的对比**：`recv_exact` 把进度放在**栈上的局部变量 `done`**，函数返回就没了（因为它保证"返回时收齐"）。事件驱动把同一个角色搬进了**活得更久的对象**——这就是状态机的本质：
   > 把本该留在栈上的局部变量，挪进一个生命周期覆盖整个连接的对象。
3. **怎么找回**：注册时 `event.data.u64 = token`，事件回来 `connections.find(token)` → `auto& connection = it->second` → `read_step(..., Connection& connection)`。**这一串 `&` 是灵魂**：值传递的话所有进度写进副本，连接永远收不满。
4. **字段分工**：

   | 字段 | 角色 |
   |---|---|
   | `header_bytes` | 头部字节缓冲（recv 直接写入） |
   | `header_done` | 头部已收字节数 = 下次续写偏移 |
   | `header` | 头收齐后 decode 出的结构化结果 |
   | `body` / `body_done` | 载荷缓冲 / 载荷进度 |
   | `phase` | 状态机阶段（header/body/sending） |
   | `output` / `sent` | 发送侧的同款结构 |
   | `last_progress` | 空闲超时用 |
   | `socket` | fd（RAII） |

5. **续写位置**：

   ```cpp
   destination = connection.header_bytes.data() + connection.header_done;  // 现算
   remaining   = connection.header_bytes.size() - connection.header_done;
   ```

   **存偏移，不存指针**：`vector` 扩容会让指针悬空；`unordered_map` rehash 时 `Connection` 会搬家，成员指针同样失效。偏移量是逻辑进度，与地址无关，天然安全。
6. **10 字节时间线**：第一次 `read_step` 写入 `[0,10)`，`header_done = 10` 后返回；第二次事件到来时 `data() + 10` 从第 11 个字节接着写。前 10 个字节一直原封不动躺在缓冲区里——`recv` 永远只往"已收之后"写，所以字节流天然连续、无空洞。

---

## Q8 响应 224 字节已发 100 字节后 EAGAIN：保留什么、等哪个事件、从哪继续发

**结论**：保留 **`output`（224 字节）和 `sent`（=100）**，等 **`EPOLLOUT`**，从 **`output.data() + sent`** 继续发。

**要点**

1. **读写的完全对偶**：

   | | 读侧 | 写侧 |
   |---|---|---|
   | 数据缓冲 | `header_bytes` / `body` | `output` |
   | 进度游标 | `header_done` / `body_done` | `sent` |
   | 等的事件 | `EPOLLIN` | `EPOLLOUT` |
   | 阻塞信号 | `recv` 返回 `EAGAIN` | `send` 返回 `EAGAIN` |
   | 应对 | return | return false |
   | 续写位置 | `data() + done` | `output.data() + sent` |

2. **为什么要预存 `output`**：一次 `send` 很可能发不完，发不完的部分必须活着等下一轮。所以 `make_response` 的结果存进 `connection.output`，而不是局部变量。
3. **EAGAIN 时什么都不改**：`send` 返回 EAGAIN 意味着**这次一个字节都没发出去**（不存在"发一半然后 EAGAIN"）。所以 `sent` 保持 100、`output` 保持 224。
4. **为什么等 `EPOLLOUT`**：它的语义是"内核发送缓冲区有空间了"，正是 EAGAIN 的反面。`EPOLL_CTL_MOD` 订阅后回去睡，内核腾出空间时叫醒你。
5. **`sent` 为什么能直接当偏移**：TCP 是**前缀语义**——`send` 返回 n 就代表前 n 个字节被内核收下，`data() + sent` 永远指向第一个还没被接受的字节。
6. **两个坑**：
   - 发完必须取消 `EPOLLOUT`（水平触发下会一直报可写 → 空转）。本项目靠关连接顺带解决；改成 keep-alive 后要手动 MOD 回 `EPOLLIN` 并复位状态；
   - `EPOLLHUP` 分支不可省：对端 RST 后可能永远等不到"正常可写"，要立即判死，不能只靠 10 秒超时兜底。

---

## Q9 `epoll_wait` 的 200ms 与空闲期限 10 秒分别控制什么？为什么慢计算拖累其他连接

**结论**：200ms 是**心跳**（`epoll_wait` 单次阻塞上限），10 秒是**生死线**（单条连接多久没 I/O 进展判死）；慢计算拖累其他连接，是因为**计算就干在单线程事件循环里面**。

**要点**

1. **200ms**（`wait_timeout_ms`）：
   - 给"等"设上限，好让循环体里的定时任务——`expire_idle`——有执行机会；
   - 没有它会怎样：对端静默消失（如拔网线，无 FIN 无 RST）→ 永远无事件 → `expire_idle` 永不执行 → fd/槽位永久泄漏；
   - **不是**"每 200ms 处理一次事件"的周期：事件来了立刻返回，它只在完全无事时才生效（打盹上限，不是工作周期）。
2. **10 秒**（`idle_timeout`）：
   - `last_progress` 在三处刷新（收到数据、图片收齐、发送有进展），衡量"最近一次真实的数据流动"，慢而活跃的客户端不会被误杀；
   - 防的是 slowloris 式慢速/僵死连接占住 8 个槽位；是应用层策略（TCP keepalive 默认 2 小时，指望不上）；
   - 精度受心跳限制：超时最多晚 200ms 被发现。
3. **为什么慢计算拖累其他连接**：`make_response`（`cv::imdecode` + 车道线检测）在事件循环**内同步执行**。它跑的时候没人在调 `epoll_wait`，其他连接的事件只能排队。
   - **内核没有丢事件、没有丢数据**：数据照样收进缓冲区、fd 照样挂进就绪链表，只是**通知被延迟消费**；
   - 量化：8 连接 × 200ms/张 ≈ 最后一个要等 1.6 秒；期间 `expire_idle` 也停摆。
4. **守则**：

   > 事件循环的回调里只做"快"的事（微秒级）。CPU 密集计算、磁盘 I/O、加锁等待都必须移出去。

5. **出路**：worker 线程池 + 任务队列 + `eventfd` 唤醒主线程；`phase` 增加 `processing`；**worker 不能直接碰 `connections` 表**（数据竞争），结果必须交回主线程处理。诚实地说：`max_connections = 8` 这种规模，线程池 + 阻塞 socket 可能更简单——epoll 的收益在"大量空闲连接"时才体现。

---

## Q10 "消息头进度 24/24" 只包含 VFLW 那 24 字节吗？头传完、图片传一半就断会怎样

**结论**：是的，只表示 **24 字节消息头到齐**，body 是独立的一笔账；图片传一半断开时，连接被清理、**半截数据永远不会进入 OpenCV**，服务器继续服务其他连接。

**要点**

1. **24 字节的构成**：`magic(4) + version(2) + type(2) + request_id(8) + payload_length(4) + flags(4)`。日志打的是 `header_done / header_bytes.size()`，此刻图片一个字节都还没收；body 有独立的 `body_done / body.size()`。
2. 必须先收齐头（`payload_length` 藏在头里）；读头时 `remaining = 24 - header_done`，**永远不会多读**进 body。`24/24` 只是字节到齐，`decode` 才校验 magic/version/type/flags/长度。
3. **三种"终止"殊途同归**：

   | 断法 | epoll 报什么 | 代码路径 | 结果 |
   |---|---|---|---|
   | 优雅关闭（FIN） | `EPOLLIN \| EPOLLRDHUP` | 剩余数据读完后 recv 得 0 → 抛"请求尚未收齐" | 关连接 |
   | 强制关闭（RST） | `EPOLLERR` | recv 得 -1/ECONNRESET → 抛"接收数据失败" | 关连接 |
   | 单纯沉默 | 什么都不报 | 10 秒后 `expire_idle` | 关连接 |

4. **半截 body 永远到不了处理器**：`make_response` 只在 `body_done == body.size()` 时调用。这是状态机的核心性质——**要么完整、要么丢弃**。能做出这个判断靠的是 VFLW 的长度前缀；纯字节流无法区分"发完"和"发一半"。
5. **资源不泄漏**：`connections.erase` → `Connection` 析构 → `SocketFd` 析构 `::close(fd)`，半个 body 的 vector 一并释放。
6. **内存是"预支"的**：头一收齐就 `body.resize(payload_length)` 分配整个缓冲，哪怕还一个字节没到。声明 16MB 只发 1 字节，也会先垫 16MB（10 秒后回收）。最坏 8 × 16MB 有界，靠的是 `decode` 里 `length > max_payload` 的校验——**没有它，一个 24 字节的头就能诱导 `resize(4GB)`，这是典型的内存放大攻击**。

---

## 速记卡

- **移动 = 所有权转移**：一个资源一生只 `close` 一次，由当时的持有者执行；源对象置空是义务，不是可选项。
- **`noexcept` 是给标准库的选路信号**：不标，容器可能退回拷贝（可拷贝类型）或丢失异常保证（move-only 类型）。
- **epoll 的本质**：把"每次挨个问所有 fd"变成"谁就绪谁举手"——O(就绪数)，与总连接数无关；LT 安全，ET 快但要求读干净。
- **`EAGAIN` 是"暂时没有"，`0` 是"永远没有"**：前者回去睡，后者按状态机决定善后。
- **状态存在对象里，续写位置 = `buffer.data() + 偏移`**：存偏移不存指针，因为 vector 会扩容、map 会 rehash。
- **事件循环里只做快的事**：任何耗时操作都会把其他连接一起拖住——内核不丢事件，只是没人及时来取。
