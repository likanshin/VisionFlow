# 第二课问答记录：异常处理与退出码

> 配套讲义：`02-image-io.md`
> 代码：`apps/vision_cli.cpp`
> 相关命令：`./build/vision_cli; echo $?`

本文件按提问顺序整理第二课的延伸讨论。讲义 2.3 节末尾提到"异常类型与引用语法以后继续展开"，这里就是那次展开。

---

## 问题 1：为什么要用 try catch？

因为 OpenCV 报告错误的方式**不统一**。它有两套并存的机制：

| 机制 | 哪些函数 | 怎么处理 |
| --- | --- | --- |
| 返回值 / 空对象 | `imread`、`imwrite` 等 I/O 函数 | 检查 `empty()`、`if (!...)` |
| 抛 `cv::Exception` | `cvtColor`、`resize`、`Mat::at<T>()` 等绝大多数处理函数 | 不接住就崩 |

当前代码里两种都写了——`image.empty()` 和 `if (!cv::imwrite(...))` 是第一种，外层的 `try`/`catch` 是第二种。

### 实测对比

把输出扩展名改成 OpenCV 不认识的 `.xyz`（`imwrite` 找不到编码器时会抛异常），编译两版对比：

**没有 try/catch：**

```text
terminate called after throwing an instance of 'cv::Exception'
  what():  OpenCV(4.2.0) .../loadsave.cpp:661: error: (-2:Unspecified error)
           could not find a writer for the specified extension in function 'imwrite_'
Aborted (core dumped)          退出码 134
```

**有 try/catch：**

```text
OpenCV error: OpenCV(4.2.0) .../loadsave.cpp:661: error: (-2:Unspecified error)
              could not find a writer for the specified extension in function 'imwrite_'
退出码 1
```

### 关键在退出码：134 vs 1

`134 = 128 + 6`，即 **SIGABRT**。异常没人接，C++ 运行时调用 `std::terminate()` 直接 abort 进程。

注意第一版里一个容易踩的坑：

```cpp
if (!cv::imwrite("build/frame_copy.xyz", image)) {   // ← 这个 if 根本没被执行
    std::cerr << "Cannot save image: ...\n";         // ← 自己写的错误提示没打印
    return 1;
}
```

**返回值检查救不了抛异常的函数。** 异常是在 `imwrite` 内部抛出的，函数压根没返回到调用处，所以精心写的错误分支一次都不会走。以为处理了错误，其实进程直接核心转储。

### try-catch 换来的三样东西

1. **可读的错误信息** —— `error.what()` 里带版本号、源文件、行号、函数名（上面那串 `loadsave.cpp:661`）。第一版虽然也打印了 `what()`，但那是 terminate handler 干的，程序变复杂或异常在别处抛出时，这个信息就拿不到了。
2. **可控的退出码** —— `1` 而不是 `134`。脚本和 CI 靠这个判断成败。
3. **不 abort** —— 真实项目里进程 abort 意味着析构函数不执行、日志不刷盘、文件没关闭。

### 补充

- `cv::Exception` 继承自 `std::exception`，由已 include 的 `cv::core.hpp` 提供。`catch (const std::exception&)` 也能接住它，覆盖面更广（顺带接住 `std::bad_alloc`、`std::out_of_range`）；接 `cv::Exception&` 则能拿到 OpenCV 特有字段（`error.code`、`error.func`、`error.file`、`error.line`）。
- 放在 `main` 里做最后兜底是常见做法。**不要**在每个函数里到处 try-catch——在边界处（`main`、线程入口）统一兜住即可。

---

## 问题 2：C++ 异常处理需要掌握的常规知识点

### 2.1 语法层：只有四个关键字

```cpp
try     { /* 可能抛异常的代码 */ }
catch (const SpecificError& e) { /* 按类型分别处理 */ }
catch (const std::exception& e) { /* 兜住标准体系 */ }
catch (...) { /* 最后的网，什么都能接 */ }
```

匹配规则是**按书写顺序，第一个能匹配的胜出**，不是"找最匹配的"。所以：

> **派生类必须写在基类前面。** 一旦先写了 `catch (const std::exception&)`，后面所有派生类分支都永远不会执行。

GCC 对这种情况会给 `-Wexceptions` 警告。`catch (...)` 只能放最后，否则编译错误。

### 2.2 铁律 1：抛异常时局部对象会被析构 —— 这是 RAII 的地基

实测输出（注意析构发生在 catch 执行**之前**）：

```text
    构造 main 的局部对象
    构造 read_frame 的局部对象
    即将 throw...
    析构 read_frame 的局部对象     ← 栈展开开始
    析构 main 的局部对象           ← 逐层向外
  catch 块开始执行: camera 0 disconnected   ← 展开完成后才进 catch
```

这就是为什么 `cv::Mat`、`std::unique_ptr`、`std::lock_guard` 能保证异常安全——它们的析构函数会在栈展开中被调用。**只要资源被对象持有，异常就不会泄漏资源。**

反过来说，裸 `new` / `malloc` / `fopen` 在异常路径上必然泄漏。

### 2.3 铁律 2：析构函数里绝不能抛异常

```cpp
struct Bad {
    ~Bad() {                                   // C++11 起析构函数隐式 noexcept
        throw std::runtime_error("析构时出错");
    }
};
```

实测结果：

```text
warning: throw will always call terminate() [-Wterminate]   ← 编译期就警告了
...
terminate called after throwing an instance of 'std::runtime_error'
  what():  析构时出错
Aborted (core dumped)        退出码 134
```

**注意：`main` 里的 `catch (const std::exception&)` 完全没用**，连它都接不住。原因：C++11 起析构函数隐式 `noexcept`，一旦有异常逃出析构函数，直接 `std::terminate()`。

如果析构过程中确实可能失败（比如 flush 缓冲区失败），正确做法是**吞掉并在析构函数内部记日志**，不要往外抛。

### 2.4 铁律 3：`throw;` 和 `throw e;` 完全不同

初看以为 `catch (std::exception e)` by value 会因为切片而丢失类型——**实测表明不会**：

```cpp
// (a) by value 形参 + throw;
catch (std::exception e) { throw; }

// (b) by value 形参 + throw e;
catch (std::exception e) { throw e; }
```

```text
  (a) throw;          -> CameraError 保住了, code=42
  (b) throw e;        -> 降级成了 std::exception: std::exception
```

原理：**`throw;` 重新抛出的是"当前正在处理的异常对象"，也就是原始那个**。by-value 的形参 `e` 只是它的一份副本，形参的存在不影响原始异常对象。所以类型、附加字段全部保留。

而 `throw e;` 是把**形参**抛出去，形参的静态类型是 `std::exception`，于是切片——不但 `code=42` 没了，连 `what()` 都退化成了 `"std::exception"`，原始错误信息彻底丢失。

> 结论：`catch (const T&)` + `throw;` 是唯一正确的重新抛出组合。**千万不要写 `throw e;`**。

### 2.5 标准异常体系

```text
std::exception                    ← what() 是 virtual const char* what() const noexcept
├── std::logic_error              ← 逻辑错误：本可以避免的编程 bug
│   ├── std::invalid_argument
│   ├── std::domain_error
│   ├── std::length_error
│   └── std::out_of_range
├── std::runtime_error            ← 运行时错误：本可避免但不可预测
│   ├── std::range_error
│   ├── std::overflow_error
│   ├── std::underflow_error
│   └── std::system_error
├── std::bad_alloc
├── std::bad_cast                 ← dynamic_cast<引用> 失败
├── std::bad_optional_access      ← .value() 访问空 optional
└── std::bad_variant_access
```

常用的触发点：

| 代码 | 抛什么 |
| --- | --- |
| `vec.at(100)` | `std::out_of_range` |
| `vec[100]` | **不抛，UB** |
| `std::stoi("abc")` | `std::invalid_argument` |
| `std::stoi("99999999999999")` | `std::out_of_range` |
| `new T`（内存不足） | `std::bad_alloc` |
| `dynamic_cast<Derived&>(base)` 失败 | `std::bad_cast` |

**自定义异常挂在 `std::runtime_error` 或 `std::logic_error` 下面**，这样即使调用者只写 `catch (const std::exception&)` 也能接住：

```cpp
class CameraError : public std::runtime_error {
public:
    int device_id;
    CameraError(const std::string& m, int id)
        : std::runtime_error(m), device_id(id) {}
};
```

必须是 **public 继承**。私有继承会让 `catch (const std::exception&)` 接不住这个异常。

### 2.6 异常安全保证

| 级别 | 含义 |
| --- | --- |
| **Nothrow（不抛保证）** | 保证不失败。`swap`、`unique_ptr` 的移动、析构函数 |
| **Strong（强保证）** | commit-or-rollback。失败时对象状态和调用前完全一致 |
| **Basic（基本保证）** | 不泄漏、对象仍可析构，但状态可能已改变 |
| **无保证** | 什么都不保证 |

强保证的经典实现是 **copy-and-swap**：

```cpp
class Image {
    cv::Mat data_;
public:
    Image& operator=(const Image& rhs) {
        Image tmp(rhs);          // 先做副本，这里失败则原对象毫发无损
        swap(*this, tmp);        // swap 是 nothrow 的，不可能失败
        return *this;
    }
};
```

一次赋值要么完整成功，要么完全没发生，没有中间状态。

### 2.7 noexcept

```cpp
void process() noexcept;                    // C++11：承诺不抛，抛了直接 terminate
bool good() const noexcept(expr);           // 条件 noexcept
static_assert(noexcept(std::swap(a, b)));   // noexcept 是运算符，可以查询
```

实用规则：

1. **移动构造函数和移动赋值应该标 `noexcept`**。否则 `std::vector` 扩容时不敢用移动，会退化成拷贝，性能掉一个量级。
2. **析构函数、`swap`、`what()` 默认就是 `noexcept`**，别去破坏它。
3. 不要随手乱标。标了就是承诺，违约 = `std::terminate`，比不标更糟。

**动态异常规范 `throw(int)` 已在 C++17 标准中移除**，看到老代码里的这种写法直接当古董处理。

### 2.8 性能：零成本模型

主流实现（Itanium ABI，GCC / Clang 都用）用的是**表驱动展开**：

- **不抛异常时：零运行时开销**。没有 `if` 判断，没有标志位设置，只有一张静态的 `.eh_frame` 表躺在二进制里。
- **代价在体积**，不在速度。表会占空间，这是嵌入式项目禁用异常的原因之一。
- **抛出时开销巨大**：栈展开 + 查表 + RTTI 匹配，微秒级。正常返回是纳秒级。

> **绝对不要用异常做正常控制流。** 比如用 `try { ... } catch (const std::out_of_range&)` 来遍历 vector 边界，或者用异常来跳出循环——这是把纳秒级操作变成微秒级。

### 2.9 什么时候该抛，什么时候用错误码

这是问题 1（OpenCV 为什么混用两套机制）的正面回答：

| 用异常 | 用错误码 / 返回值 |
| --- | --- |
| 构造函数失败（**构造函数没有返回值**，这是异常存在的核心理由） | 可预期、高频发生的错误 |
| 运算符重载无法返回错误码 | 文件不存在、网络超时 |
| 错误无法就地处理，必须往上传播 | 正常业务分支 |
| 目标代码（算法、库内部） | 性能敏感的热路径 |

现代 C++ 的替代品：`std::optional`（可能没有值）、`std::error_code`（系统错误）、C++23 的 `std::expected<T, E>`（有值或错误）。

### 2.10 陷阱清单

**必踩的：**

- 析构函数抛异常 → `terminate`（铁律 2）
- 异常逃出 `main` → `terminate`，退出码 134
- **异常逃出线程入口函数 → `terminate`**。线程函数必须自己兜住所有异常，否则整个进程挂掉
- `catch` 写成 by value → 多一次拷贝，且拿不到派生类成员
- `throw e;` 重新抛出 → 切片（铁律 3）
- 基类 `catch` 写在派生类前面 → 派生类分支变死代码
- `catch (...)` 里不重新抛也不记日志 → 错误被静默吞掉，最难查的 bug

**认知类的：**

- **C++ 没有 `finally`。** 用 RAII 或 scope guard 代替
- `goto` 不能跨过 `try` / `catch` 块
- 全局 / 静态对象**不会**在栈展开中被析构，它们只在程序正常退出时析构
- 构造函数抛异常时，**该对象自己的析构函数不会执行**，但已完整构造的基类和成员会。所以资源要用 RAII 成员持有
- `catch` 的参数对象在 `catch` 块结束时析构——它也是栈展开的一部分

**进阶（暂时了解即可）：**

- `std::exception_ptr` / `std::current_exception()` / `std::rethrow_exception()` —— 跨线程搬运异常。`std::async` 的 `future::get()` 就是靠它把子线程的异常在主线程重新抛出来的
- `std::throw_with_nested` / `std::nested_exception` —— 保留异常链，不丢失原始错误
- `std::uncaught_exceptions()`（C++17，注意有 s）—— scope guard 判断是否在展开中

### 2.11 回看 `apps/vision_cli.cpp`

```cpp
} catch (const cv::Exception& error) {     // ✅ by const reference
    std::cerr << "OpenCV error: " << error.what() << "\n";   // ✅ 打印 what()
    return 1;                              // ✅ 可控退出码
}
```

这段是教科书式的正确写法。唯一可以改进的是覆盖面——`cv::Exception` 只接得住 OpenCV 的异常。想连 `std::bad_alloc`、`std::out_of_range` 一起兜住，可以补一层：

```cpp
} catch (const cv::Exception& error) {
    std::cerr << "OpenCV error: " << error.what() << "\n";
    return 1;
} catch (const std::exception& error) {   // 顺序正确：派生类在前
    std::cerr << "Error: " << error.what() << "\n";
    return 1;
}
```

对本课范围来说，当前这版已经够用。

---

## 问题 3：`echo $?` 是什么意思？

### 定义

`$?` 是 shell 的特殊变量，保存**上一条命令的退出状态码**（exit status）。`echo $?` 把它打印出来。

这是 Unix 最基础的进程间约定：**每个进程结束时都会返回一个 0–255 的整数**，`0` 表示成功，非 0 表示失败。

### 特殊含义的码

| 值 | 含义 |
| --- | --- |
| `0` | 成功 |
| `1`–`125` | 命令自己定义的错误（由程序决定） |
| `126` | 找到了命令，但无法执行（权限不足） |
| `127` | 命令不存在 |
| `128 + N` | **被信号 N 杀死** |

信号那一条最重要，展开：

| 退出码 | 信号 | 场景 |
| --- | --- | --- |
| `130` | `128+2` SIGINT | **按了 Ctrl+C** |
| `134` | `128+6` SIGABRT | `abort()`、未捕获异常 |
| `137` | `128+9` SIGKILL | `kill -9`、OOM killer 杀进程 |
| `139` | `128+11` SIGSEGV | **段错误**（空指针、数组越界） |
| `141` | `128+13` SIGPIPE | 管道下游已关闭 |
| `143` | `128+15` SIGTERM | 常规的 `kill` |

看到 139 就是程序崩了，看到 134 就是 `terminate` / abort——都跟"正常报错退出"是两回事。

### 坑 1：它会被立刻覆盖

```bash
false
echo "紧跟着 false: $?"     # 1
echo "再问一次:    $?"     # 0  ← 上一条 echo 自己成功了
```

`$?` 只记录**紧邻的上一条**命令。而且 `$?` 是在 `echo` 启动**之前**由 shell 展开的，所以 `echo $?` 打印的是上一条命令的状态，不是 `echo` 自己的。

想在脚本里用，必须马上存起来：

```bash
some_command
status=$?          # ✅ 立刻存
if [ "$status" -ne 0 ]; then ...
```

### 坑 2：超出 255 会取模

退出状态只有一个字节（8 位），实际存的是 `值 & 0xFF`：

```text
exit 256 -> 0        # 256 % 256 = 0，失败变成了成功！
exit 257 -> 1
exit 300 -> 44       # 300 % 256 = 44
exit -1  -> 255      # 有符号数转成无符号
```

所以 `return -1;` 出来是 255，`exit 256` 出来是 0——**经典的"错误被静默吃成成功"**。C++ 里 `return` 的取值范围老老实实控制在 0–125 最安全。

### 接回 C++

把这一串串起来，前面几个演示的退出码就全说通了：

| C++ 代码 | 发生了什么 | `$?` |
| --- | --- | --- |
| `return 0;` | 正常结束 | `0` |
| `return 1;` | catch 块里可控地失败 | `1` |
| 异常逃出 `main` | `terminate()` → `abort()` → SIGABRT | `134` |
| 空指针解引用 | SIGSEGV | `139` |
| 运行中按 Ctrl+C | SIGINT | `130` |

这就是为什么问题 1 里说，**try-catch 的价值之一是"把 134 变成 1"**。返回 `1` 说明"程序自己知道出错了，并告诉了你原因"；`134` 说明"程序失控了，什么都没来得及说"。

注意 `apps/vision_cli.cpp` 里 `return 1;` 一共出现在**三处**：`image.empty()` 分支（第 13 行）、`imwrite` 失败分支（第 23 行）、以及 `catch` 块（第 30 行）。前两处在 `try` 内部**直接返回，根本不经过 `catch`**——`catch` 只管第三种情况：OpenCV 主动抛异常。这正是问题 1 里"返回值检查救不了抛异常的函数"的另一面：两条错误路径彼此独立，缺一不可。

### 实际用途

```bash
# 1. 条件判断
make -j8 && ./build/vision_cli || echo "构建或运行失败"

# 2. 脚本里检查
if ! ./build/vision_cli; then
    echo "vision_cli 失败，退出码 $?"
    exit 1
fi

# 3. CI 靠它判断成败——GitHub Actions 里某个 step 返回非 0，整个 job 就算失败

# 4. 调试崩溃
./build/vision_cli; echo "退出码: $?"   # 看到 139 就知道该上 gdb 了
```

---

## 附：速查表

### 异常处理关键字

| 关键字 | 作用 | 关键约束 |
| --- | --- | --- |
| `throw e;` | 抛出新异常 | 不要抛裸类型 / 裸指针；重新抛出时**会切片** |
| `throw;` | 重新抛出当前异常 | 保留原始类型，只能在 `catch` 内用 |
| `try` | 标记受保护区间 | |
| `catch (const T&)` | 按类型捕获 | **必须 by const reference**；派生类写在基类前 |
| `catch (...)` | 兜住一切 | 只能放最后；不要静默吞掉 |
| `noexcept` | 承诺不抛 | C++11 起析构 / `swap` / `what()` 默认为它 |

### 退出码速查

| 码 | 含义 | 怎么来的 |
| --- | --- | --- |
| `0` | 成功 | `return 0;` |
| `1` | 通用失败 | `return 1;`（本课的两个错误分支） |
| `126` | 不可执行 | 权限不足 |
| `127` | 命令不存在 | 路径写错 |
| `130` | 被 SIGINT 中断 | Ctrl+C |
| `134` | 被 SIGABRT 杀死 | 未捕获异常、`abort()` |
| `137` | 被 SIGKILL 杀死 | `kill -9`、OOM |
| `139` | 被 SIGSEGV 杀死 | 段错误 |

### 术语表

| 术语 | 含义 |
| --- | --- |
| 栈展开（stack unwinding） | `throw` 后逐层析构局部对象直到匹配的 `catch`，RAII 的地基 |
| 切片（object slicing） | 派生类对象被按基类类型拷贝，丢失派生部分。`throw e;` 和 by-value `catch` 是来源 |
| 零成本模型 | 不抛异常时无运行时开销，代价体现在二进制体积（`.eh_frame` 表） |
| 异常安全保证 | Nothrow / Strong / Basic 三级；强保证用 copy-and-swap 实现 |
| exit status | 进程结束时返回给 shell 的 0–255 整数，`$?` 读它；超出 255 取模 |
