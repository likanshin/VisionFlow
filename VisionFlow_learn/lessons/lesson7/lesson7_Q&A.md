# 第七模块 Q&A：套接字所有权、TCP 数据流与跨语言通信

整理日期：2026-10-02

涉及文件：`include/tcp_io.hpp`、`src/tcp_io.cpp`、`include/protocol.hpp`、`src/protocol.cpp`、`apps/vision_server.cpp`、`python/vision_client.py`

本篇是阅读 `07-1-blocking-tcp.md` 过程中提出的六个问题的整理。每个问题按「结论 → 展开 → 与本项目代码的对应」组织，可以单独查阅。

---

## 1. 为什么 `SocketFd` 要删除拷贝构造函数和拷贝赋值运算符？

对应代码：`include/tcp_io.hpp:13-14`

```cpp
SocketFd(const SocketFd&) = delete;
SocketFd& operator=(const SocketFd&) = delete;
```

### 1.1 结论

因为 `SocketFd` 是**资源的所有者**（RAII 类），它独占一个 socket 文件描述符。如果允许拷贝，就会出现「两个对象以为自己拥有同一个 fd」，析构时**重复关闭**（double close）——这是网络编程里最难查的一类 bug。

### 1.2 默认拷贝构造做了什么

编译器生成的拷贝构造函数是**逐成员浅拷贝**：

```cpp
SocketFd(const SocketFd& other) : fd_(other.fd_) {}  // 编译器隐式生成的版本
```

`fd_` 只是一个 `int`，拷贝的就是那个**数字**，没有任何新的内核资源被创建：

```cpp
SocketFd a(sock);     // fd_ = 7
SocketFd b = a;       // b.fd_ 也 = 7 —— 两个对象都认为"7 号 fd 归我管"
```

文件描述符的所有权天然不可共享（除非显式做引用计数），所以这在语义上是错的。

### 1.3 为什么这是灾难性的

**（a）双重关闭。** 两个对象析构时都执行 `::close(fd_)`，第二次返回 `EBADF`。

**（b）fd 复用导致"误杀无辜"。** 内核分配 fd 的规则是**取当前最小的可用编号**，所以 fd 号会被循环利用：

```cpp
SocketFd a(7);
{
    SocketFd b = a;          // b 和 a 共享 7 号
}                            // b 析构 → close(7)，真的关掉了连接
// ... 期间程序别处又 open()/accept() 了一个连接,内核把 7 号分给了它 ...
// a 析构 → close(7)，关掉了一个完全无关的连接
```

这类 bug 表现为「某个不相关的连接在随机时刻莫名断开」，出错点和肇事点相隔很远，加日志也难以定位。

**（c）悬空句柄。** `a` 关闭后 `b` 还握着失效的 fd 号，`recv(b.get(), ...)` 要么 `EBADF`，要么（在 fd 被复用后）**读写到别的连接上**，数据串流比崩溃更可怕。

所以 `= delete` 的实质是：**把「所有权只能独占、不能复制」这件事在类型系统层面表达出来**。

### 1.4 为什么是 `= delete`，而不是别的写法

| 写法 | 效果 |
|---|---|
| 不声明 | 编译器自动生成，直接出错 |
| 声明为 `private` 且不实现 | 类外调用报「是私有成员」，错误信息绕 |
| 继承 `boost::noncopyable` | 可行，但引入依赖，报错也不直观 |
| **`= delete`（C++11）** | 编译期报错，信息清晰：*use of deleted function* |

两个细节：

- **`= delete` 的函数仍参与重载决议**。它不是「不存在」，而是「存在但被删除」，所以编译器能精准地在调用拷贝构造那一行报错。
- **放在 `public` 下是故意的**。C++ 的访问检查发生在 deleted 检查**之前**；若放到 `private`，报错会变成「这个函数是私有的」，反而让人摸不着头脑。

### 1.5 Rule of Three / Rule of Five

> 如果你需要自己写**析构函数**（说明类在管理某种资源），那你几乎一定需要自己处理**拷贝构造**和**拷贝赋值**——要么删除、要么实现深拷贝、要么实现移动。

编译器生成的版本永远做浅拷贝，而浅拷贝对「资源持有者」几乎总是错的。

注意当前代码的一个副作用：**因为显式声明了析构函数和拷贝构造（哪怕是 delete），移动构造和移动赋值也不会被隐式生成了**（[class.copy.ctor]/8）。所以这个类型目前**既不能拷贝也不能移动**：

```cpp
SocketFd f(int fd) { SocketFd s(fd); return s; }   // 编不过
SocketFd g(int fd) { return SocketFd(fd); }        // C++17 保证的拷贝消除（RVO）才行
std::vector<SocketFd> v;  v.push_back(...);        // 编不过
std::optional<SocketFd> o;                         // 编不过
```

若希望它能被移交（放进容器、传给工作线程），应补上移动语义，把「转移所有权」也表达出来：

```cpp
SocketFd(SocketFd&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;                 // 源对象放弃所有权,变成"空"状态
}
SocketFd& operator=(SocketFd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);  // 先释放自己原有的资源
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}
// 对应地,析构必须是: if (fd_ >= 0) ::close(fd_);
```

`-1` 作为「无资源」哨兵值是 POSIX 惯例（`open` 失败就返回 -1）。这正是 `std::unique_ptr` 的做法：**不可拷贝、可移动**。`SocketFd` 本质上就是「文件描述符版的 `unique_ptr`」。

### 1.6 什么时候拷贝反而是对的

- **想共享同一个连接** → 不要拷贝 `SocketFd`，用 `std::shared_ptr<SocketFd>`，让引用计数管理生命周期；
- **想转移所有权** → 用移动（见上）；
- **想复制底层资源** → 那应该叫 `dup()` 或 `clone()`，是一个**显式命名的成员函数**（调用 `::dup(fd_)` 拿到新 fd 号），而不是伪装成拷贝构造。

一句话：fd 是独占资源，拷贝 = 两个主人 = 重复释放。`= delete` 把这条规则从注释里的君子协定变成了编译器强制执行的契约，让错误在**编译期**暴露，而不是在运行期的随机时刻。

---

## 2. 这相当于单例模式吗？

### 2.1 结论

不是。两者只是**恰好用了同一个技巧**（删除拷贝构造），**目的完全不同**。

### 2.2 两者约束的不变量不同

**单例模式**的不变量是：整个程序里这个类只能存在 **1 个实例**。

```cpp
class Config {
public:
    static Config& instance() {   // 全局唯一的访问点
        static Config c;          // 整个程序只构造这一次
        return c;
    }
    Config(const Config&) = delete;              // 和 SocketFd 长得一样
    Config& operator=(const Config&) = delete;   // 和 SocketFd 长得一样
private:
    Config() = default;           // 构造函数私有:外面根本造不出来
};
```

这里的 `= delete` 是为了堵死「绕过 `instance()` 私自复制出第二个 Config」的路。**限制的是实例个数。**

**`SocketFd`** 的不变量是：每个 fd 编号恰好有**一个所有者**。

```cpp
SocketFd a(sock1);   // 可以
SocketFd b(sock2);   // 可以
SocketFd c(sock3);   // 可以，造多少个都行,一个连接一个
SocketFd d = a;      // 不行：那 7 号 fd 就有两个主人了
```

**限制的是所有权的复制，不是实例的个数。** 一个服务器有 1000 个客户端，就应该有 1000 个 `SocketFd`。

### 2.3 最直观的反驳

如果 `SocketFd` 真是单例，那意味着**整个程序只能有一条 TCP 连接**——第二个客户端连上来时，连一个表示它的对象都造不出来。而单例的 `Config`/`Logger` 恰恰就是想要这个效果。

### 2.4 泄露天机的细节：构造函数是 public 的

```cpp
explicit SocketFd(int fd);   // public,而且明确要求你传一个 fd 进来
```

单例的第一特征是**构造函数私有**，必须走 `instance()` 唯一入口。而 `SocketFd` 的构造函数是 public 的，还特意用 `explicit` 防止隐式转换——**这本身就在鼓励你造很多个**。

### 2.5 对比表

| | 单例 `Config` | `SocketFd` |
|---|---|---|
| 不变量 | 全程序只有 1 个实例 | 每个 fd 只有 1 个所有者 |
| 实例数量 | 恰好 1 | 任意多个（一连接一个） |
| 构造函数 | `private` + `static instance()` | `public` + `explicit SocketFd(int)` |
| 删除拷贝的原因 | 防止复制出第二个实例 | 防止两个对象持有同一 fd，导致 double close |
| 从属范畴 | **设计模式**（对象数量 / 全局访问点） | **RAII / 所有权语义**（资源生命周期正确性） |
| 关注的问题 | 全局状态怎么组织 | 资源什么时候、被谁释放 |
| 现代 C++ 地位 | 常被视为反模式（隐式全局状态、难测试） | 核心惯用法，无处不在 |

### 2.6 直觉对了一半

两者确实都在保证某种「唯一性」，区别在于**唯一的是什么**：

- 单例：**全局**唯一 —— 整个世界就这一个。
- `SocketFd`：**每资源**唯一 —— 可以有很多个，但「7 号 fd 的主人」只能有一个。

用比喻说：**单例**像「一个国家只能有一位总统」，限制的是**岗位数量**；**SocketFd** 像「一把钥匙只能有一个人持有」，限制的是**副本数**，而钥匙可以有很多把。`SocketFd` 里真正唯一的是那个 `int fd_` 代表的内核资源，对象只是它的持有凭证。

### 2.7 更贴切的类比：`std::unique_ptr`

| | `std::unique_ptr<T>` | `SocketFd` |
|---|---|---|
| 持有 | 一个 `T*` | 一个 `int` fd |
| 析构时 | `delete ptr` | `close(fd)` |
| 拷贝 | 删除 | 删除（同一理由：双重释放） |
| 移动 | 支持，源置 `nullptr` | 目前还没写（见第 1.5 节） |

### 2.8 `= delete` 是手段，不是模式

删除拷贝构造这个技巧出现在很多互不相关的地方：

- **单例** —— 保证全局唯一性；
- **`unique_ptr` / `thread` / `mutex` / `fstream` / `SocketFd`** —— 保证独占所有权；
- **含引用成员或 `const` 成员的类** —— 编译器隐式删除，连写都不用写。

判断一个类属于哪种情况，要看它的**不变量**（在保护什么），而不是看它用了哪些语法。

---

## 3. 为什么服务端用 `accept` 返回的描述符收发，而不用监听描述符？

### 3.1 结论

因为**监听 fd 和 accept 返回的 fd 在内核里是两个不同的对象**：前者是「接客的门」，后者才是「某一个具体的客人」。监听 fd 上根本没有数据可读——它连「对端是谁」都不知道。

### 3.2 类比：前台和分机

- `socket()` + `bind()` + `listen()` 创建的是**总机/前台**（监听 fd），守在某个端口上；
- `accept()` 是接线员把某个客户**转接**给你，并给你一个**分机号**（新的 fd）；
- 之后你只能对着分机号说话，对着前台喊话客户听不到。

### 3.3 硬核理由：fd 是一张「一对一」的表

在 Linux 里，`fd` 本质上是进程私有的一个数组下标，指向内核中的 `struct file` → `struct socket` → `struct sock`。**一个 fd 号只能指向一个内核 socket 对象**，不存在「一个 fd 背后装着 N 条连接、可以任意取一条来读」这种东西。

- 监听 socket 对应的 `struct sock`，状态是 `TCP_LISTEN`。它的接收队列里装的是**已完成三次握手、排队等着被 accept 的连接**（accept queue / 全连接队列），**不是字节流数据**。
- 每 accept 一次，内核从队列里取出一个连接，**克隆出一个全新的 `struct sock`**，并分配一个新的、当前最小的可用 fd 号返回。这条连接的序列号、窗口、重传定时器、收发缓冲区都挂在这个**新对象**上。

所以「用监听 fd 收发」不是程序员的约定，而是**内核数据结构上的不可能**。真对监听 socket 调 `recv()`/`send()`，内核会直接返回错误（Linux 上是 `ENOTCONN` 一类）。

### 3.4 「连接」的身份是四元组

一条 TCP 连接由**四元组**唯一标识：

```text
(客户端IP, 客户端端口, 服务端IP, 服务端端口)
```

- **监听 fd**：只知道自己绑的 `(服务端IP, 服务端端口)`，**完全没有对端信息**，无法唯一确定任何一条连接；
- **accept 出来的 fd**：绑定到一条具体四元组。`getpeername()` 查对端，`getsockname()` 查本地端口。

由此得到一个反直觉的事实：**同一个端口上可以同时存在成百上千个 socket**。区分它们的不是端口，而是四元组——`ss -tn` 里几十行 Local Address 完全相同，每行是一条独立连接。

### 3.5 代码形状与生命周期

```cpp
int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
bind(listen_fd, ...);
listen(listen_fd, /*backlog=*/128);   // 进入 LISTEN 状态,开始能接客

while (true) {
    int conn_fd = accept(listen_fd, ...);   // 每轮产生一个新的 conn_fd
    recv(conn_fd, buf, ...);                // 收:针对这一条连接
    send(conn_fd, resp, ...);               // 发:针对这一条连接
    close(conn_fd);                         // 这条连接结束,关掉它
}
// listen_fd 全程只创建一次,循环内绝不收发数据、绝不关闭
```

| | 监听 fd | accept 的 fd |
|---|---|---|
| 数量 | 程序里 1 个（每个监听端口一个） | 每条连接 1 个 |
| 生命周期 | 进程级别，通常只 `close` 一次 | 连接级别，连接结束就 `close` |
| 能 `recv`/`send` 吗 | 不能，内核拒绝 | 能，这才是它的用途 |
| 对应内核对象 | 连接工厂 + 待 accept 队列 | 一条具体连接的完整状态 |

### 3.6 精度细节：握手完成后数据会丢吗

不会。三次握手完成后，即使服务端**还没**调用 accept，客户端发来的数据也会被内核接收并缓存在新生成的 socket 接收队列里。

但那个 socket 的 fd 是**在 accept 时才分配**的。数据在，但你没有句柄去读它。

顺带一提，`listen(fd, backlog)` 里的 `backlog` 限制的就是「已完成握手但还没被 accept 取走」的队列长度。队列满了新连接会被丢弃，客户端表现为超时——这就是高并发服务必须尽快 accept、再交给别的线程处理的原因。

### 3.7 回到 `SocketFd`

- **每个 `SocketFd` = 一条连接 = 一个内核对象**，所以它必须独占（不可拷贝），这就是 `tcp_io.hpp:13-14` 那两行的原因；
- `recv_exact(int fd, ...)` / `send_all(int fd, ...)` 的 `fd` 参数，**语义上永远是 accept 出来的连接 fd**，绝不应该是监听 fd；
- `apps/vision_server.cpp:29` 用 `SocketFd listener` 包住监听 fd，`:49` 用 `SocketFd connection` 包住 accept 出来的 fd——两者都能靠析构自动 `close`，但生命周期完全不同。

---

## 4. 需要接收 24 字节头，第一次 `recv` 只得到 10 字节，下次从哪继续、最多还需接收多少？

### 4.1 直接答案

- **继续写的位置**：`data + 10`（缓冲区第 10 号下标，第 11 个字节）
- **最多还需接收**：`24 - 10 = 14` 字节

`src/tcp_io.cpp:22-35` 的写法正是这个意思：

```cpp
const auto count = ::recv(fd, data + done, size - done, 0);
//                              ↑ 写入位置      ↑ 这次最多要多少
```

代入 `size = 24`、第一次 `done` 从 0 变成 10：

```cpp
::recv(fd, data + 10, 14, 0);   // 从第 10 字节开始写,这次最多读 14 字节
```

### 4.2 逐步推演

| 轮次 | `done` | 调用参数 | 假设返回 | 调用后 `done` | 还缺 |
|---|---|---|---|---|---|
| 1 | 0 | `recv(fd, data+0, 24)` | 10 | 10 | 14 |
| 2 | 10 | `recv(fd, data+10, 14)` | 14 | 24 | 0（退出循环） |
| 2' | 10 | `recv(fd, data+10, 14)` | 6 | 16 | 8 |
| 3' | 16 | `recv(fd, data+16, 8)` | 8 | 24 | 0（退出循环） |

`data + done` 和 `size - done` 这两个表达式是通用写法的关键：`done` 既表示「已经填到哪」，也表示「还差多少」（`size - done`）。所以它能自动适应任意次数的短读，不管对端把 24 字节切成几段。

### 4.3 为什么是「最多」14 字节

因为它**刚好等于还缺的量**，一个字节都不能多要：

1. **可能越界写（UB）**。若调用方传的是 `std::uint8_t header[24]`，要 15 字节以上就是往数组外写内存。`recv_exact` 是通用函数，不知道 `data` 后面还有多少合法空间，所以**唯一安全的请求量就是 `size - done`**。

2. **即使缓冲区更大，多要也是错的**。假设协议是 `[24 字节头][N 字节体]`，缓冲区是 24 + N。这次要了 100 字节，内核会把体的前 86 字节一起塞进头区域。头是收齐了，但**下一段体少了 86 字节，再也找不回来**——协议解析从这一帧开始彻底错位。

代价是内核当前只有 3 字节可读时就得循环多次。但这是必须付的代价：TCP 没有消息边界，你没法「顺便多拿点存着」，除非在应用层实现带缓冲区的解析器。

### 4.4 关键概念：`recv` 返回 10 不代表对端只发了 10

返回值只说明：在调用那一刻，内核这个 socket 的接收缓冲区里**恰好有 10 字节可以给你**。

那 24 字节可能是：

- **被拆开到达的**——对端一次 `send(24)`，但网络分片传输，前 10 字节先到；
- **对端本来分两次写的**——先 `send(10)` 再 `send(14)`；
- **对端写了一次，但内核分批交付**。

反过来，「两次 `send` 被一次 `recv` 全收走」也完全可能，这就是**粘包**。

所以 `recv` 的返回值是「**这次给了我多少**」，不是「**对端发了多少**」。TCP 只保证**字节顺序**，不保证**消息边界**——这正是 `recv_exact` 存在的全部理由：把「流」重新拼回「消息」。

### 4.5 `count == 0` 必须处理

`recv` 返回 0 表示对端已 `close`（发了 FIN），此时数据还没收齐就是**连接提前断开**，必须抛错。若不处理，`done` 永远不增长，会变成死循环。`src/tcp_io.cpp:28-29` 的处理是正确的。

---

## 5. Python 里的 `with ... as ...` 是什么意思？

对应代码：`python/vision_client.py:33`

```python
with socket.create_connection(("127.0.0.1", 19090)) as connection:
```

### 5.1 结论

`with ... as ...` 是 **Python 版的 RAII**——和 C++ 里 `SocketFd` 的析构函数是同一个思想：**块结束时自动释放资源**，而且**无论正常结束还是中途抛异常**都保证释放。

这一行的含义：建立连接 → 绑定到名字 `connection` → 之后无论 `vision_client.py:34-55` 里发生什么，离开这个缩进块时**一定会 `connection.close()`**。

### 5.2 它等价于什么

```python
# 实际写法
with socket.create_connection(("127.0.0.1", 19090)) as connection:
    connection.sendall(request)
    ...

# 完全等价的展开写法
connection = socket.create_connection(("127.0.0.1", 19090))
try:
    connection.sendall(request)
    ...
finally:
    connection.close()      # 不管有没有异常,都会执行
```

`as` 后面那个名字没什么魔法，就是把 `with` 表达式的结果绑定成普通变量。`with` 真正做的事，是**替你写好了那个 `try/finally`**。

### 5.3 背后机制：上下文管理协议

`with` 要求对象实现两个「魔术方法」：

```python
class MyResource:
    def __enter__(self):
        # 进入 with 块时调用
        return self      # 这个返回值才是绑定给 `as` 名字的东西

    def __exit__(self, exc_type, exc_value, traceback):
        # 离开 with 块时调用,正常/异常都会走这里
        self.close()
        return False     # False:异常继续往外抛;True:吞掉异常
```

`socket` 对象自己就实现了这两个方法，`__exit__` 里干的就是 `close()`。

`__exit__` 收到的三个参数是异常信息：没异常时三个都是 `None`。返回 `True` 相当于「这个异常我处理了，别往外传」——绝大多数情况下应该返回 `False` 或什么都不返回。

### 5.4 为什么这里**必须**用 `with`

`vision_client.py` 的 `with` 块里有多处可能抛异常：第 39、41、43、45、49、52 行的 `ValueError`，第 36 行的 `ConnectionError`（来自 `recv_exact`），第 47 行的 `json.JSONDecodeError`，第 36 行的 `struct.error`。

这些异常都被第 61 行统一捕获。如果不用 `with` 而是手动 `connection.close()`，只要在它之前任何一处 `raise`，那个 `close()` 就永远执行不到——**fd 泄漏**。

单次运行看不出来（进程退出时 OS 会回收所有 fd），但循环处理上百个请求的客户端会累积泄漏，最终撞上 `EMFILE: too many open files`。

> 容易踩的误区：CPython 有引用计数，对象没人引用时会立刻回收，看起来「不加 with 也会自动关」。但这是 **CPython 的实现细节**，不是语言保证——PyPy、有循环引用时、JIT 环境下关闭时机都不确定。官方文档明确推荐用 `with`。

### 5.5 和 C++ RAII 对照

```cpp
// C++
{
    SocketFd connection(::accept(...));
    send_all(connection.get(), request, n);
    ...
}   // 右大括号:栈展开,~SocketFd() 自动 close(fd_)
```

```python
# Python
with socket.create_connection(("127.0.0.1", 19090)) as connection:
    connection.sendall(request)
    ...
# 缩进结束:__exit__ 自动 close()
```

| | C++ | Python |
|---|---|---|
| 界定边界的东西 | **作用域** `{ }` | **with 块** |
| 获取资源 | 构造函数 / RAII 对象 | `__enter__` |
| 释放资源 | 析构函数 `~T()` | `__exit__` |
| 异常时的保证 | 栈展开（stack unwinding）调用析构 | 解释器调用 `__exit__` |
| 谁来触发 | 编译器，隐式、无法绕过 | 语法层面，必须记得写 `with` |

关键差异：**C++ 是隐式的**——只要对象在栈上就躲不掉析构；**Python 是显式的**——必须主动写 `with`，忘了写就回到手动管理的世界。这是因为 Python 没有 C++ 那种确定性的析构时机，所以语言给了你一个手动划定作用域的工具来模拟它。

### 5.6 不止 socket：到处都在用

```python
with open("data.json") as f:              # 文件,离开自动 f.close()
    text = f.read()

with threading.Lock():                    # 锁,等价于 C++ 的 std::lock_guard
    shared_counter += 1

with subprocess.Popen(cmd) as proc:       # 子进程
    ...
```

### 5.7 自己写一个上下文管理器

手动写两个方法：

```python
class Timer:
    def __enter__(self):
        self.start = time.monotonic()
        return self
    def __exit__(self, *exc):
        print(f"耗时 {time.monotonic() - self.start:.3f}s")

with Timer():
    do_heavy_work()
```

更常用的简写是 `contextlib`：

```python
from contextlib import contextmanager

@contextmanager
def timer():
    start = time.monotonic()
    try:
        yield                      # yield 之前 = __enter__,之后 = __exit__
    finally:
        print(f"耗时 {time.monotonic() - start:.3f}s")
```

### 5.8 两个语法点

```python
with open("a.txt") as f:      # 需要对象本身 → 写 as
with lock:                    # 不需要对象 → 省略 as
with open("in") as fi, open("out", "w") as fo:   # 多个资源一行搞定(等价于嵌套)
```

---

## 6. 为什么 `recv_exact(connection, HEADER.size)` 能接收到 C++ 服务端返回的数据？

对应代码：`python/vision_client.py:36` 与 `apps/vision_server.cpp:110`

### 6.1 结论

因为 **Python 进程和 C++ 进程之间根本没有「互相调用」**。它们谁都不认识对方，也没有共享任何内存，各自只跟**同一个操作系统的内核**打交道——数据是通过**内核的 socket 缓冲区**传递的字节流。

`recv()` 这一句做的事情，准确说是：

> 「把**我自己进程的**内核接收缓冲区里的 24 个字节，拷贝到我的用户态内存里。」

而 C++ 服务端 `send_all()` 做的事情是：

> 「把这 24 个字节，从我的用户态内存拷贝到**内核的发送缓冲区**里。」

内核负责把后者变成前者。

### 6.2 完整的数据通路

```text
C++ 进程 用户态：  message 字节数组 (24字节头 + JSON体)
      │
      │  net::send_all(conn_fd, ...)        ← 系统调用
      ▼
内核：该连接的【发送缓冲区】
      │
      │  TCP 协议栈:切分、编号、滑动窗口
      │  127.0.0.1 走回环设备,不经过物理网卡
      ▼
内核：该连接的【接收缓冲区】               ← 这就是 connection 指向的地方
      │
      │  connection.recv(24)                ← 系统调用
      ▼
Python 进程 用户态：  header = recv_exact(connection, 24)
```

关键点：**C++ 端可能早就把数据发完、甚至进程已经退出**，数据还安静地躺在内核缓冲区里。Python 不是「向 C++ 请求数据」，而是在读**自己的**内核缓冲区。这就是为什么两边不需要同时运行，也不需要任何「互操作框架」。

回环地址 `127.0.0.1` 值得说一句：数据确实不经过物理网卡，但**照样完整走一遍 TCP 协议栈**（分段、序号、ACK、流量控制一个不少）。所以它不是共享内存，依然会出现短读——这正是 `recv_exact` 那个循环存在的原因。

### 6.3 两个 socket 是两个不同的东西

| | C++ 服务端 | Python 客户端 |
|---|---|---|
| 对象 | `SocketFd connection`（`accept` 出来的，`vision_server.cpp:49`） | `connection`（`create_connection` 返回） |
| 本质 | 内核里一个 `struct sock` | 内核里**另一个** `struct sock` |
| 谁创建的 | 服务端内核，从监听队列取出 | 客户端内核，三次握手后建立 |

它们是**两个独立的内核对象**，靠 TCP **四元组**关联成一条连接。一个是上游端，一个是下游端。`header_bytes` 和 `header` 这两个变量之间没有任何语言层面的通道——它们之间隔着内核。

### 6.4 那「认识」靠什么？靠字节的约定

内核只搬运字节，它**不知道**什么 `uint64_t`、什么 `struct`。所以两边必须自己约定：**这 24 个字节，每一位分别是什么意思。**

| 偏移 | 长度 | 字段 | C++ 侧 | Python 侧 |
|---|---|---|---|---|
| 0 | 4 | 魔数 `"VFLW"` | `protocol.cpp:31` | `4s` |
| 4 | 2 | 版本 = 1 | `protocol.cpp:32` | `H` |
| 6 | 2 | type（1/2/3） | `protocol.cpp:33` | `H` |
| 8 | 8 | request_id | `protocol.cpp:34` | `Q` |
| 16 | 4 | payload_length | `protocol.cpp:35` | `I` |
| 20 | 4 | flags = 0 | 数组初始化即 0 | `I` |
| — | **24** | 合计 | `header_size = 24` | `HEADER.size = 24` |

Python 的 `struct.Struct("!4sHHQII")` 逐字符对应这张表：`4s`=4 字节、`H`=2 字节、`Q`=8 字节、`I`=4 字节，加起来正好 **24**。

### 6.5 代码刻意避开的两个跨语言大坑

**（a）字节序（endianness）。** C++ 的 `put()` 函数（`src/protocol.cpp:11-16`）从低位往高位填、把最低字节写在最后一个位置——刻意构造成**大端**（网络字节序）；Python 侧的 `!` 也是大端，两边一致。

如果 C++ 图省事直接 `memcpy` 一个 `uint64_t`，在小端 CPU（x86/ARM）上字节顺序就是反的——Python 解析出的 `request_id` 会变成天文数字，`vision_client.py:42` 的检查会立刻报错。

**（b）结构体 padding（填充字节）。** C++ 里的 `struct Header` **不能**直接 `memcpy` 出去。编译器为了内存对齐，会在 `uint16_t type` 和 `uint64_t request_id` 之间插入填充字节，还会在结构体尾部补齐——字段的实际偏移和协议约定的偏移**完全对不上**，填充字节内容还是不确定的垃圾。

所以 `src/protocol.cpp` 选择了**手工逐字段序列化**：`encode()` 一字节一字节地写，`decode()` 一字节一字节地读。这叫**序列化**，是跨语言通信的必备功课。Python 侧 `struct` 模块的 `!` 前缀同样禁用了对齐填充，两边才严丝合缝。

> 这也是为什么 C++ 侧用 `std::uint16_t`/`std::uint64_t`（`include/protocol.hpp:19-23`），而不是 `short`/`long`——`long` 在 Linux 64 位上是 8 字节，在 Windows 上是 4 字节，跨平台跨语言必须用**宽度确定的类型**。

### 6.6 数据还没到怎么办

`connection.recv()` 默认是**阻塞**模式。如果此刻内核接收缓冲区是空的（比如 C++ 那边还在跑 OpenCV 车道线检测），这个函数**不会返回**，而是把 Python 进程挂起、放进睡眠队列，直到：

- 有数据到达 → 内核唤醒它，返回读到的字节数；
- 对端 `close()` → 返回 `0`（`vision_client.py:17-18` 就处理这种情况）；
- 出错 → 返回 `-1` 并设置 `errno`。

所以 Python 端「傻等」是设计如此，它在等内核通知它「数据来了」。

### 6.7 为什么是「先读 24，再读 length」

服务端 `vision_server.cpp:107` 用 `protocol::packet()` 把头 + 体拼成**一个连续 buffer**，一次 `send_all` 全发出去。但客户端却分**两段**读：先读 24 字节头（第 36 行），从头里解析出 `length`，再读 `length` 字节体（第 46 行）。

这不是多此一举——因为 TCP 是**字节流，没有消息边界**。服务端一次 `send` 的 1000 字节，到了客户端可能变成 3 次 `recv`，也可能和下一次 `send` 粘在一起。客户端根本不知道「一条消息到哪结束」，所以必须有个**定长的头**在开头写明后面还有多长。

这就是第 4 节讲的「头定长、体变长」的 framing 方案，也是世界上几乎所有二进制协议（HTTP/2、gRPC、Redis RESP）的共同做法。

---

## 附：本篇关键概念速查

| 概念 | 一句话 |
|---|---|
| RAII | 资源在构造函数中获取、析构函数中释放，用作用域保证成对 |
| `= delete` | 声明「存在但禁止调用」，编译期报错，比 private 报错更清晰 |
| Rule of Three/Five | 写了析构就要同时考虑拷贝/移动，编译器只会做浅拷贝 |
| 监听 fd vs 连接 fd | 门 vs 房间；连接 fd 才绑定四元组、才有收发缓冲区 |
| 四元组 | `(客户端IP, 客户端端口, 服务端IP, 服务端端口)`，TCP 连接的身份 |
| 短读（short read） | `recv` 一次不保证收满，返回值只是「这次给了多少」 |
| 粘包 | 多次 `send` 可能被一次 `recv` 合并，TCP 不保留消息边界 |
| framing | 用定长头声明变长体长度，把字节流切回消息 |
| 阻塞 I/O | 缓冲区空时调用不返回，进程睡眠直到数据到达 |
| 上下文管理器 | Python 的 `with`，用 `__enter__`/`__exit__` 实现 RAII |
| 字节序 | 本协议统一用大端（网络字节序），C++ 手工填、Python 用 `!` |
| 序列化 | 逐字段手工编解码，绕开结构体 padding 和对齐的不确定性 |
