# 第六课问答记录：自定义 namespace、constexpr、enum class 与 protocol.cpp 精读

> 配套讲义：[06-1-vflw-protocol.md](06-1-vflw-protocol.md)
> 代码：[protocol.hpp](../../../include/visionflow/protocol/protocol.hpp)、[protocol.cpp](../../../src/protocol/protocol.cpp)、[server.cpp](../../../src/net/server.cpp)、[core_tests.cpp](../../../tests/unit/core_tests.cpp)
> 涉及数据流：`Header`（内存）→ `encode` → 24 协议字节 → `packet` → 网络 → `decode` → `Header`（内存）

本文件按提问顺序整理第六课的五次问答。

> **说明**：本文的代码引用与分析针对原项目 `src/protocol/protocol.cpp`（[VFLW 讲义](06-1-vflw-protocol.md)要求你在 `VisionFlow_learn/` 下写的是同一份实现）。文中所有 `nm` / `objdump` 的输出均来自本机实际构建产物 `build/debug/.../protocol.cpp.o`，不是示意。

---

## 问题 1：介绍一下自定义 namespace

### 一句话：namespace 是"给名字加前缀"这件事的语言级实现

在 C 里，如果两个库都有一个 `encode`，只能靠人工约定：

```c
// 库 A
void vflw_protocol_encode(...);
// 库 B
void jpeg_encode(...);
```

C++ 说：与其靠人自觉写前缀，不如让编译器帮你加。于是 `namespace visionflow::protocol { void encode(...); }` 里的 `encode`，**真名**叫 `visionflow::protocol::encode`。前缀是**语法**的一部分，写错就是编译错误，不靠自觉。

### 为什么真的需要它：链接器层面

假设没有 namespace，你的 [protocol.hpp:16](../../../include/visionflow/protocol/protocol.hpp#L16) 和另一个库都定义了 `encode(const Header&)`。编译能过（两个 .cpp 分开编译，互相看不见），**链接时才炸**：

```text
multiple definition of `encode(Header const&)'
```

因为 C++ 里"函数名"在链接器眼中的唯一标识是 mangled name，而它**包含 namespace**。本机实际构建产物的证据：

```text
$ nm build/debug/.../protocol.cpp.o
00000000000000a8 T _ZN10visionflow8protocol6encodeERKNS0_6HeaderE
00000000000001c4 T _ZN10visionflow8protocol6decodeEPKhmj
000000000000004e t _ZN10visionflow8protocol12_GLOBAL__N_13getEPKhm
0000000000000000 t _ZN10visionflow8protocol12_GLOBAL__N_13putEPhmm
```

读一下 `_ZN10visionflow8protocol6encodeE...`：`10visionflow` 是**长度前缀 + 名字**（Itanium ABI 的编码方式），`8protocol` 同理，`6encode` 是函数名，`E` 是名字结束。**namespace 被一字一句编进了符号名**——两个库各自的 `encode` 在链接器看来根本是两个东西。

每行开头的字母也有含义：

| 字母 | 含义 |
| --- | --- |
| `T` | 大写 = **外部链接**，全局可见 |
| `t` | 小写 = **内部链接**，只在当前 .o 里可见 |

`put` / `get` 是小写的 `t`，因为它们在匿名 namespace 里（见下文 4.4）。

### 四种用法

#### 1. 定义（可以反复打开）

```cpp
namespace visionflow::protocol {   // C++17 嵌套写法
// ...
} // namespace visionflow::protocol
```

C++17 之前必须写成两层花括号：`namespace visionflow { namespace protocol { ... } }`。`A::B {}` 只是语法糖。

关键性质：**namespace 是"开放"的**。头文件里声明、.cpp 里定义之所以能对上，是因为 [protocol.hpp:8](../../../include/visionflow/protocol/protocol.hpp#L8) 和 [protocol.cpp:5](../../../src/protocol/protocol.cpp#L5) 的 `namespace visionflow::protocol {` 是**同一个 namespace 被打开了两次**，不是两个东西。这点和 class 完全不同（class 不能拆开重开）。

#### 2. 限定名

```cpp
visionflow::protocol::Header h;
h.type = visionflow::protocol::Type::image;
```

想显式表达"全局的那个"时用开头的 `::`：`::encode(...)`。

#### 3. `using` 声明 / `using` 指令——两者差别很大

```cpp
using visionflow::protocol::encode;   // 声明：只引入 encode 这一个名字
using namespace visionflow::protocol; // 指令：整个 namespace 的名字都可见
```

| | `using X::f;` | `using namespace X;` |
| --- | --- | --- |
| 引入范围 | 只有 `f` | X 里所有名字 |
| 能引入重载集合 | 能 | 能 |
| 影响面 | 小、可控 | 大、隐式 |

**别名**是第三种东西，它不引入任何名字，只是给长名字起短名：

```cpp
namespace protocol = visionflow::protocol;   // 之后写 protocol::encode(...)
```

#### 4. 匿名 namespace

```cpp
namespace visionflow::protocol {
namespace {
void put(std::uint8_t* out, std::uint64_t n, std::size_t count) { ... }
} // namespace
```

没有名字的 namespace。编译器会给它生成一个**每个 .cpp 都不相同**的内部名字——上面 `nm` 输出里的 `12_GLOBAL__N_1` 就是它。效果：

- 函数 / 变量 / **类型**都变成**内部链接**，别的 .cpp 链接不到
- 不会和别的文件里的同名 `put` 撞车（即使对方也在匿名 namespace 里，那串唯一名不同）

这基本是 C 里 `static` 的 C++ 版本，但更强——`static` 只能管住函数和变量，匿名 namespace 连 `struct`、`class`、`constexpr` 常量都能管住。**不需要对外暴露的实现细节，全部塞进匿名 namespace。**

### 实践规则和坑

#### ① 头文件里永远不要 `using namespace`

`#include` 是文本替换，头文件里的 `using namespace std;` 会传染给**每一个**包含它的 .cpp。头文件里一切用全限定名写清楚。

#### ② 别往 `std` 里加东西

```cpp
namespace std { void my_func(); }   // 未定义行为
```

唯一例外是给**你自己的类型**特化标准模板（`std::hash<MyType>` 之类），那也必须写在 `namespace std` 里。

#### ③ namespace 名用小写，避免全大写——因为宏不认 namespace

```cpp
namespace data { int size; }
#define size 42
data::size   // 被替换成 data::42 → 编译错误
```

预处理器是**纯文本替换**，`data::size` 里的 `size` 照样被换掉，namespace 一点保护作用都没有。所以 namespace 名用小写、不以 `_` 或 `__` 开头（保留给实现）。

#### ④ 没有运行时开销

namespace 完全在编译期解决，不生成任何代码、不占运行时空间。编译出的机器码和"把所有东西写在全局、靠 `xxx_` 前缀区分"一模一样。**纯零成本抽象。**

#### ⑤ 补充：ADL（参数依赖查找）

```cpp
std::cout << header;   // 为什么能找到 operator<<？
```

编译器除了正常查找，还会**去实参所属的 namespace 里找**同名函数。所以在 `visionflow::protocol` 里定义了 `operator<<(std::ostream&, const Header&)`，调用方不用 `using` 也能用上。这叫 ADL（也叫 Koenig 查找）。

#### ⑥ 进阶：inline namespace

```cpp
namespace visionflow {
inline namespace v2 { void encode(...); }
}
```

`inline` 的 namespace 对外**透明**——调用方写 `visionflow::encode` 就能用。这是做 ABI 版本化的手段（libstdc++ 内部大量使用）。写业务代码基本用不到。

### 落到本课代码

1. **对外接口** → 具名 namespace（`visionflow::protocol`），保证跨文件、跨库不撞名
2. **实现细节** → .cpp 里的匿名 namespace，让链接器帮你"藏起来"

养成写结尾注释 `} // namespace visionflow::protocol` 的习惯，大文件里这个注释能救命。

---

## 问题 2：constexpr 是什么意思

### 一句话

`constexpr` = **constant expression**，"这个值在**编译期**就算出来了"。

### 和 `const` 的分水岭：什么时候算出来的

两个关键词回答的是**两个不同问题**：

| | 回答的问题 |
| --- | --- |
| `const` | 值**能不能改**（只读） |
| `constexpr` | 值**什么时候知道**（编译期） |

```cpp
int n = read_from_file();   // 运行期才知道
const int a = n;            // ✅ 合法。a 只读，但运行期才确定
constexpr int b = n;        // ❌ 编译错误：n 不是常量表达式
```

`const` **不承诺时间**，它只说"以后不能改"。`constexpr` 承诺了时间——所以它更强：既只读，又必须能在编译期算出结果。编译器会**强制检查**这一点。

### 为什么非要"编译期"不可

C++ 里有一批位置**只收编译期常量**，填运行期的值是语法错误：

- 数组 / `std::array` 的元素个数
- 模板参数
- `case` 标签
- `enum` 的初值
- `static_assert` 的条件
- 位域宽度

本课代码正好踩中第一个。

### 自己的代码就是活教材

[protocol.hpp:9](../../../include/visionflow/protocol/protocol.hpp#L9) 定义常量，[protocol.hpp:16](../../../include/visionflow/protocol/protocol.hpp#L16) 把它当模板参数：

```cpp
constexpr std::size_t header_size = 24;
std::array<std::uint8_t, header_size> encode(const Header& header);
```

编译产物里有三条证据。

#### ① 值被烙进了**类型名**

```text
std::array<unsigned char, 24ul>::data()
std::array<unsigned char, 24ul>::begin()
std::array<unsigned char, 24ul>::end()
```

那个 `24ul` 不是运行期存着的数字，它是**类型本身的一部分**。`std::array<uint8_t, 24>` 和 `std::array<uint8_t, 25>` 是**两个毫不相干的类型**，不能互相赋值。这就是为什么元素个数必须是编译期常量——否则编译器连"这是个什么类型"都定不下来。

#### ② 值被烙进了**指令**

`encode` 的反汇编（debug 构建，没开优化）里字节偏移全是硬编码立即数：

```asm
movq $0x0,(%rax)      # 清 24 字节 = header_size
movq $0x0,0x8(%rax)
movq $0x0,0x10(%rax)
mov    $0x4,%esi      # 4 = "VFLW" 的长度
add    $0x4,%rax      # +4 = magic 之后的偏移
add    $0x6,%rax      # +6 = type 字段之后
```

`header_size` 在函数体里**根本不存在**，只剩立即数。**零运行时开销**——和手写 `24` 生成的机器码完全一样。

#### ③ `header_size` 自己在 .o 里长什么样

```text
0000000000000000 r visionflow::protocol::header_size
```

- 小写 `r` = 在 **.rodata 只读数据段**（对比 `T`/`t` 是代码段）
- 原始符号名里有玄机：`_ZN10visionflow8protocolL11header_sizeE`，那个 **`L`** 是 Itanium ABI 里"**内部链接**"的标记

也就是说：**命名空间作用域的 `constexpr` 变量默认是内部链接的**（因为它隐含 `const`），每个 .cpp 各有一份副本，链接器不会报符号冲突。

### 第二形态：`constexpr` 函数

```cpp
constexpr std::uint32_t max_payload() {
    return 16U * 1024U * 1024U;      // 编译器直接算成 16777216
}
```

含义是"**这个函数允许在编译期求值**"，**不是"只能在编译期调用"**。同一个函数有两副面孔：

- 实参都是常量表达式，且用在需要常量的位置 → 编译期算完，结果是个常量
- 实参是运行期的值 → 退化成普普通通的运行期函数

所以 `constexpr` 是"**至少能**在编译期算"，真正的"只能"是 C++20 的 `consteval`。

版本演进：C++11 的 `constexpr` 函数限制极严（函数体基本只能一条 `return`），C++14 才放宽到可以有循环、局部变量、多分支。本项目是 C++17，随便用。

### 第三形态：`if constexpr` —— [config.cpp:44](../../../src/core/config.cpp#L44) 就在用

```cpp
auto read = [&](const char* key, auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_integral_v<T>) {
        if (!j[key].is_number_integer() || j[key].get<long double>() < 0
            || j[key].get<long double>() > static_cast<long double>(std::numeric_limits<T>::max()))
            throw std::invalid_argument(...);
    }
    value = j[key].get<T>();
};
```

这是个泛型 lambda，`T` 可能是 `std::uint16_t`，也可能是 `std::string`。想一下**如果这里是普通的 `if`**：普通 `if` 的两个分支**都必须能编译**。当 `T = std::string` 时，`std::numeric_limits<std::string>::max()` 返回一个 `std::string`，`static_cast<long double>(std::string)` 直接编译失败——哪怕运行期根本不会走这个分支。

`if constexpr` 就是为这个场景发明的：**条件在模板实例化时不成立，那个分支就被整个丢弃。**

一个精确的边界：这个"丢弃分支不需要合法"的能力**只在模板里成立**。写在非模板函数里时，两个分支仍然都必须合法（只是没被选中的那个不生成代码）。

### 顺便修正 lesson5 的一处说法

[lesson5_Q&A.md:232](../lesson5/lesson5_Q&A.md#L232) 写的是：

> `inline constexpr` 是 C++17 的写法——头文件里定义变量要加 `inline`，否则多个 `.cpp` 包含它会重复定义。

对普通全局变量（`int kMax = 256;`）这句完全正确。但对 `constexpr` 变量，失败模式**不是**链接错误——它是**内部链接**，压根不会跟别的 .cpp 撞车。

那为什么还要加 `inline`？因为它隐含的 `const` 带来两个副作用：

1. 每个 .cpp 各拿一份**独立的副本**（多浪费一点空间）
2. 更麻烦的是：在 inline 函数里取它的地址，各 TU 会拿到**不同的地址** → ODR 违规，编译器通常不报错，是未定义行为

`inline` 的作用是取消内部链接，让它变成**全程序唯一**的实体。所以 `inline constexpr` 仍然该写，只是理由不是"避免重复定义"，而是"避免出现多份副本"。

### `constexpr` vs `#define`

[config.cpp:52](../../../src/core/config.cpp#L52) 刚用了 `#define VF_READ(name)`，正好对比：

| | `#define MAX 256` | `constexpr int kMax = 256;` |
| --- | --- | --- |
| 本质 | 文本替换（预处理器） | 真变量，有类型 |
| 作用域 | 无，从定义处到文件尾 | 正常作用域规则 |
| 命名空间 | **完全不认**（宏会穿透 namespace） | 正常 |
| 调试器 | 看不见 | 看得见 |
| 类型检查 | 无 | 有 |

能用 `constexpr` 的地方就不要用 `#define`——宏是 C 时代没有别的选择时的工具。

> **实践建议**：默认写 `constexpr`。它是"最强的 `const`"，同时把"这个值编译期就知道"告诉编译器和读代码的人。只有确实需要运行期才能确定的值，才退回 `const`。

---

## 问题 3：`array<uint8_t, header_size>` 与 `array<uint8_t, 24>` 有区别吗？没区别为什么不直接写 24？

### 编译后：完全没有区别

不是"差不多"，是**一模一样**。`nm` 输出的类型就是：

```text
std::array<unsigned char, 24ul>::data()
```

`header_size` 在编译期就被替换成 `24`，之后它就不存在了。同一个类型、同一份机器码、零开销。**这不是性能问题，也不是代码生成问题。**区别**全在人这一侧**。

### 这是"一个 24"还是"十四个 24"

`header_size` 在原项目里的分布：

| 文件 | 出现处 |
| --- | --- |
| [protocol.hpp](../../../include/visionflow/protocol/protocol.hpp) | [:9](../../../include/visionflow/protocol/protocol.hpp#L9) 定义、[:16](../../../include/visionflow/protocol/protocol.hpp#L16) 模板参数 |
| [protocol.cpp](../../../src/protocol/protocol.cpp) | [:19](../../../src/protocol/protocol.cpp#L19)、[:20](../../../src/protocol/protocol.cpp#L20)、[:29](../../../src/protocol/protocol.cpp#L29)、[:43](../../../src/protocol/protocol.cpp#L43) |
| [server.cpp](../../../src/net/server.cpp) | [:38](../../../src/net/server.cpp#L38)、[:49](../../../src/net/server.cpp#L49)、[:155](../../../src/net/server.cpp#L155)、[:195](../../../src/net/server.cpp#L195)、[:197](../../../src/net/server.cpp#L197)、[:208](../../../src/net/server.cpp#L208)、[:209](../../../src/net/server.cpp#L209)、[:272](../../../src/net/server.cpp#L272) |

**14 处，跨 4 个文件。**

写成 `24` 就是 14 个互不相干的 24。假设协议升级——加一个 4 字节的 CRC 字段，`header_size` 变成 28，版本号从 1 升到 2（[protocol.cpp:31](../../../src/protocol/protocol.cpp#L31) 那个 `get(p + 4, 2) != 1` 正好会用到）：

- 用 `header_size`：改 **1 处**，另外 13 处自动跟上
- 写 `24`：改 **14 处**

### 关键不是"改起来累"，是"漏一处会怎样"

漏改的后果**严重程度不一样**：

- 漏了 [server.cpp:38](../../../src/net/server.cpp#L38) 的接收缓冲区大小 → 缓冲还是 24 字节，`decode` 要 28 → 抛 `"invalid header size"`，**当场炸给你看**，还算幸运
- 漏了 [server.cpp:272](../../../src/net/server.cpp#L272) 的包体切片偏移 → 从错误位置取数据，**不报错，只是数据是错的**
- 漏了 [server.cpp:197](../../../src/net/server.cpp#L197) 的 `header_size - header_used` → 解析进度算错，同样**不报错**

**沉默的错误才是成本。** 常量提取把"14 处必须保持一致"从**人的记忆**变成了**编译器的职责**。

### 更深一层：常量提取是在给数字一个"身份"

> **"碰巧都是同一个数" vs "本来就是同一个数"**

[protocol.cpp:34](../../../src/protocol/protocol.cpp#L34) 有个现成的例子：

```cpp
if (type < 1 || type > 3) throw std::runtime_error("unknown message type");
```

这两个数**必须**和 [protocol.hpp:10](../../../include/visionflow/protocol/protocol.hpp#L10) 的枚举保持一致：

```cpp
enum class Type : std::uint16_t { image = 1, result = 2, error = 3 };
```

现在往枚举里加一个 `heartbeat = 4`——校验就**静默地**变成错的：合法的 heartbeat 消息会被拒绝。没有任何编译器警告。这跟 `header_size` 是**同一种病**：两处代码必须手工同步，而没有东西强制它们同步。

再看 [protocol.cpp:22-25](../../../src/protocol/protocol.cpp#L22-L25)，`4`、`6`、`8`、`16`、`20` 全是魔法数字，它们和 `header_size = 24` 描述的是**同一张布局表**：

```text
0-3    magic "VFLW"      4 字节
4-5    version           2
6-7    type              2
8-15   request_id        8
16-19  payload_length    4
20-23  flags             4
                          = 24 = header_size
```

那要不要把这些也全提取成 `offset_version`、`offset_type`……？**不一定，这里有真实的反向权衡**：这是**线格式（wire format）**，这些偏移由协议规范冻结，除非发 v2 否则永远不会变。而 `header_size` 不一样，它跨 4 个文件被用作缓冲区大小、切片长度、比较基准——**同一个数值被赋予了多种用途**，这才是它值得有名字的原因。

判断标准大致三条，满足越多越该提取：

1. **用了不止一次**（否则直接写更清楚）
2. **语义相同**（数值相同但含义不同，恰恰**不该**共用——那是把两个概念错误地绑在一起）
3. **可能要改**（比如从枚举推导的范围值）

顺手给布局加一条"冻结"声明，一行换永久保护：

```cpp
static_assert(header_size == 24, "wire layout is frozen; bump version to change it");
```

### 直接回答"为什么不直接写 24"

因为 `constexpr` 常量**不是给编译器用的，是给未来的你和读代码的人用的**。编译器压根不需要名字——它在编译期就把名字扔了。需要名字的是：

- **改代码的人**：只改一处，而不是四处找、八处核对
- **读代码的人**：`header_size` 自解释，`24` 得靠猜
- **搜代码的人**：`grep header_size` 一击命中，`grep 24` 会翻出一堆无关的 24

> 一句话：**`constexpr` 是零成本抽象——它对运行时收 0 元，却把人这一侧的一致性负担交给了编译器。**

---

## 问题 4：解释一下 `enum class`

### 一句话

`enum class` = **有作用域、有类型**的枚举，C++11 引入，专门用来替掉 C 的老式 `enum`。

### 先看它治好了什么病

```cpp
enum Type { image, result, error };   // C 风格
```

三个毛病，全是"编译器不拦你"型的：

```cpp
int x = image;                 // ① 隐式转整数，合法
int y = Type::error;           // ② 名字直接泄漏，image 到处都能写
if (some_color < Type::image)  // ③ 两个不相干的枚举能比大小，合法
```

③ 尤其荒唐——"红色小于图片类型"是什么语义？但编译器一声不吭。

[protocol.hpp:10](../../../include/visionflow/protocol/protocol.hpp#L10) 的写法：

```cpp
enum class Type : std::uint16_t { image = 1, result = 2, error = 3 };
//   ^^^^^^^^  ^^^^  ^^^^^^^^^^^^^^   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//   有作用域   名字    底层类型              枚举值
```

- 必须写 `Type::image`，裸写 `image` 编译错误 → 名字不泄漏
- `int x = Type::image;` **编译错误**，必须 `static_cast<int>(...)` → 不会偷偷变成整数
- 不同类型的枚举不能互相比较 → ③ 写不出来

### 那个 `: std::uint16_t` 是干嘛的

这是**底层类型**（underlying type）。不写默认是 `int`（通常 4 字节）；写了 `std::uint16_t`，就精确是 2 字节。

对协议代码这不是洁癖，是刚需——VFLW 规定 type 字段占 2 字节。**而且它真的生效了**，`encode` 的反汇编：

```asm
129:  mov    -0x30(%rbp),%rax     # rax = &header
12d:  movzwl (%rax),%eax          # 读 h.type
```

`movzwl` = **mov z**ero-extend **w**ord to **l**ong——那个 `w` 就是读 16 位。如果底层类型是默认的 `int`，这里会是 `mov (%rax),%eax`（读 4 字节）。`: std::uint16_t` 一字不差地落到了机器码上。

### 但是：底层类型对了 ≠ 可以直接发内存

从同一段反汇编还原出的 `Header` **内存**布局（`movzwl (%rax)` 读 type、`mov 0x8(%rax),%rbx` 读 request_id、`mov 0x10(%rax),%eax` 读 payload_length）：

```text
内存里的 Header                        线上的 24 字节
偏移 0    type           2 字节         0-3    magic "VFLW"
偏移 2    [填充]         6 字节         4-5    version
偏移 8    request_id     8 字节         6-7    type        ← 在 6，不在 0
偏移 16   payload_length 4 字节         8-15   request_id
偏移 20   [填充]         4 字节         16-19  payload_length
        ──────────────────────          20-23  flags
        sizeof(Header) = 24                    = 24 = header_size
```

**两个都是 24 字节。** 这纯属巧合——但巧得很危险：

```cpp
std::memcpy(buf, &header, sizeof(header));   // ❌ 长度刚好对得上
```

长度对得上，不越界，不崩溃，编译器不警告。但发出去的内容是：magic 变成了栈上的垃圾、version 变成垃圾、type 落进了填充区。`request_id` 和 `payload_length` 倒是**碰巧位置一致**，更容易让人误以为"好像也没错太多"。

sizeof 相同只是巧合，真正的障碍有两条：

1. **字节序**：结构体按本机字节序存（x86 是小端），协议要求大端
2. **填充字节**：那 6 字节和 4 字节填充的内容是**未定义的**，取决于编译器和栈上残留数据——发出去等于泄漏内存内容

正确做法就是 `protocol.cpp` 里做的：**逐字段显式 `put()`**，取枚举值时只用 `static_cast<std::uint16_t>(h.type)` 拿它的**值**，绝不动它的**内存**。

### 最有价值的一段：decode 里的"信任边界"

```cpp
const auto type = get(p + 6, 2);        // ① 外部数据 → 裸整数
if (type < 1 || type > 3) throw ...;    // ② 用整数校验
return {static_cast<Type>(type), ...};  // ③ 校验通过 → 变成强类型
```

这个顺序**是被类型系统逼出来的**，而这正是它最好的用法：

第 ② 步的 `type < 1` 如果 `type` 声明成 `Type`，根本编译不过（enum class 不和整数比大小）。所以你**必须**先用裸整数接住外部数据、校验完，再 cast。

于是代码里自然形成一道关卡：**"未经验证的外部字节"和"可信的强类型值"是两个不同的类型**。每一处 `static_cast<Type>` 都是你在声明"我担保这里的数据合法"。`grep static_cast<Type>` 就能找出全部的信任边界。

对比 [server.cpp:210](../../../src/net/server.cpp#L210)，进入内部以后就没这层负担了：

```cpp
if (connection.parsed.type != protocol::Type::image) throw ...;
```

### 一个必须知道的坑：类型安全 ≠ 值域安全

```cpp
auto t = static_cast<Type>(999);   // 合法！编译、运行都不报错
```

`Type` 里只定义了 1、2、3，但你可以造出任意值的 `Type`。枚举**不是**值域受限的整数——它只是给整数贴了个标签，编译器只检查"标签类型对不对"，**不检查"值合不合法"**。

所以 [protocol.cpp:34](../../../src/protocol/protocol.cpp#L34) 那句范围检查**一个都不能省**。安全保证只覆盖"类型"这一层，"值"这一层永远得自己守。

### 几个配套知识点

#### ① 可以前置声明（头文件里很有用）

```cpp
enum class Type : std::uint16_t;   // OK
enum Old : std::uint16_t;          // 老式 enum 也可以，但底层类型必须写
enum Old;                          // ❌ 老式 enum 这样不行
```

#### ② 不能有成员函数，配套写法是自由函数 + `switch`

```cpp
constexpr const char* to_string(Type t) {
    switch (t) {
        case Type::image:  return "image";
        case Type::result: return "result";
        case Type::error:  return "error";
    }
}
```

这里藏着 `enum class` 最实用的福利：**只要 switch 不写 `default` 分支，编译器（gcc/clang 的 `-Wswitch`）会检查你是否覆盖了所有枚举值**。哪天往 `Type` 里加 `heartbeat = 4`，所有没处理它的 switch 立刻报警。

对比 [protocol.cpp:34](../../../src/protocol/protocol.cpp#L34) 的 `if (type < 1 || type > 3)`——加一个 `heartbeat`，它**一声不吭**地就错了。同一个"两处必须同步"的问题，`switch` 版本编译器帮你盯，`if` 范围版本只能靠人。

#### ③ `using enum`（C++20）

可以在局部把 `Type::` 省掉。本项目是 C++17，用不了。

#### ④ 可以作非类型模板参数

`std::array<uint8_t, static_cast<std::size_t>(Type::image)>` 是合法的，枚举常量本身就是常量表达式。

> **总结**：`enum class` 干的事就一件——**把"一个整数"升级成"一个有身份的类型"**，然后在所有需要显式转换的地方留下痕迹，让你能看见、能 grep、能审计。和 `constexpr` 思路完全一致：**把人的记忆负担转成编译器的检查义务。**

---

## 问题 5：详细解释 `src/protocol.cpp` 的内容

这份 47 行的文件是整个协议模块的核心。`put`/`get` 是"私有层"，另外三个函数是"公开层"。

先放**线格式表**，后面所有 `put(out.data() + N, ...)` 和 `get(p + N, ...)` 里的 `N` 都对应它：

| 偏移 | 长度 | 字段 | 谁写 |
| --- | --- | --- | --- |
| 0 | 4 | magic `VFLW` | encode 硬编码 |
| 4 | 2 | version = 1 | encode 硬编码 |
| 6 | 2 | type | `Header` 成员 |
| 8 | 8 | request_id | `Header` 成员 |
| 16 | 4 | payload_length | `Header` 成员 |
| 20 | 4 | flags = 0 | encode 靠清零 |
| | | | 合计 24 = `header_size` |

### 第 1 段：include（1-5 行）

```cpp
#include "visionflow/protocol/protocol.hpp"
#include <algorithm>
#include <limits>
```

**第一行 include 自己的头文件，是必须养成的习惯。** 如果 `protocol.hpp` 漏了某个 include，编译这个 .cpp 会立刻炸——错误暴露在你自己的文件里，而不是等到某个无辜的调用方才发现。这叫头文件"自包含"。

**一个值得知道的细节**：这个 .cpp 用了 `std::array`、`std::vector`、`std::runtime_error`、`std::length_error`、`std::size_t`——这些**都不是** `<algorithm>` 和 `<limits>` 提供的，它们来自 hpp 里 include 的 `<array>` / `<vector>` / `<stdexcept>` / `<cstddef>`。这叫**依赖传递包含**。严格按规范，用到哪个名字就该 include 哪个头；实践中"依赖自己头文件的传递包含"风险很小（因为头文件本身必须自包含），所以这么写可以接受，但你应该**知道自己依赖了什么**。

`<algorithm>` 提供 `std::copy_n` 和 `std::equal`；`<limits>` 提供 `std::numeric_limits`。这两个是 .cpp 独有的需求。

`namespace visionflow::protocol {` 打开命名空间——和 hpp 里**是同一个**，这就是问题 1 讲的"namespace 可以重开"。

### 第 2 段：put / get（6-18 行）—— 大端序的两个对称操作

藏在匿名 namespace 里，外部看不见，是整个文件的发动机。

#### `put`：把一个整数**倒着**写进缓冲区

```cpp
void put(std::uint8_t* out, std::uint64_t n, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        out[count - i - 1] = static_cast<std::uint8_t>(n & 255U);
        n >>= 8U;
    }
}
```

三个参数：写到哪（`out`）、写什么（`n`）、写几个字节（`count`）。用一个 `uint64_t` 接住所有宽度，所以 1 到 8 字节共用一个函数。

注意 `out[count - i - 1]` 这个下标——**从末尾往前填**。跟着算一遍 `put(buf, 0x0102, 2)`：

| i | `n & 255U` | 写到哪 | 之后 n |
| --- | --- | --- | --- |
| 0 | `0x02`（最低位） | `out[1]` ← 最后 | `0x01` |
| 1 | `0x01` | `out[0]` ← 最前 | `0` |

结果 `01 02`——高位在前，正是大端。**关键洞察**：每次取的是**最低 8 位**（`n & 255U`），而最低位在大端序里应该排**最后**，所以倒着填是必然的，不是随意选择。

两个细节：

- `255U` 的 `U` 后缀保证是无符号字面量，避免有符号/无符号混算的坑
- `static_cast<std::uint8_t>` 必须写：`n & 255U` 的类型是 `uint64_t`，赋给 `uint8_t` 是**窄化**。显式 cast 是在说"我清楚我在砍掉高位，而且此刻高位一定是 0"

**一个必须知道的隐患**：`put` **不检查 `count` 是否装得下 `n`**。`put(p, 70000, 2)` 会把 `70000`（`0x11170`）的低 16 位 `0x1170` 静默写进去，**不报错**。这个前提由调用者保证——encode 里四次调用的宽度（2/2/8/4）和字段宽度严格对应，所以安全。

#### `get`：**正着**读回来

```cpp
std::uint64_t get(const std::uint8_t* p, std::size_t count) {
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < count; ++i) n = (n << 8U) | p[i];
    return n;
}
```

`n << 8` 把已有数字往左推 8 位腾出空位，`| p[i]` 把新字节塞进最低位。`get({0x01, 0x02}, 2)`：

| i | 运算 | n |
| --- | --- | --- |
| 0 | `0 << 8 \| 0x01` | `0x01` |
| 1 | `0x0100 \| 0x02` | `0x0102` |

和 `put` 完全对称：**put 从末尾往前用最低位，get 从开头往后攒高位**。同一个协议两头对称，这是它们能互逆的原因。

统一返回 `uint64_t`，调用方自己 cast 到目标类型——和 `put` 接受 `uint64_t` 也是对称的。

#### 为什么放匿名 namespace

见问题 1：内部链接、不污染符号表、跨 .cpp 不会撞名。

### 第 3 段：encode（19-27 行）

```cpp
std::array<std::uint8_t, header_size> encode(const Header& h) {
    std::array<std::uint8_t, header_size> out{};      // ← 这个 {} 是全场焦点
    std::copy_n("VFLW", 4, out.begin());
    put(out.data() + 4,  1, 2);                                    // version
    put(out.data() + 6,  static_cast<std::uint16_t>(h.type), 2);   // type
    put(out.data() + 8,  h.request_id, 8);                         // request_id
    put(out.data() + 16, h.payload_length, 4);                     // payload_length
    return out;
}
```

**返回 `std::array` 而不是 `std::vector`**：24 是类型的一部分，编译期就确定，**不需要堆分配**。

#### `out{}` 那对花括号是关键

看四行 `put` 覆盖了哪些偏移：0-3、4-5、6-7、8-15、16-19。**20-23 没人写**——那是 flags，协议要求恒为 0。

谁负责让它为 0？就是这个 `{}`。`std::array` 的值初始化会把 24 个字节**全部清零**，于是 flags 天然是 0。

**如果没有这个 `{}`**，那 4 个字节就是**未初始化的栈垃圾**——可能碰巧是 0，也可能是上一个函数遗留的敏感数据。而这个返回值会被原样发到网络上。这不是"可能出错"，这是**内存泄漏漏洞**。

编译器把它优化成了三条 8 字节写（就是问题 2 里那三条 `movq $0x0`）。

#### `std::copy_n("VFLW", 4, out.begin())`

`"VFLW"` 的字面量类型其实是 `const char[5]`——**末尾还带一个 `\0`**。`copy_n` 只拷前 4 个，正好避开它。这就是为什么用 `copy_n` 而不是 `strcpy`：不依赖结尾符，长度是明确的。

#### `put(out.data() + 4, 1, 2)` 里的 `1`

version 被硬编码成 1，因为**它根本不是 `Header` 的成员**。回想讲义第 4 节的设计：变化的字段进结构体，固定的字段由编码函数负责。这个设计让调用者**不可能**构造出"版本号错误的 Header"——`Header` 里压根没有 version 这个字段可填。**用类型系统消灭一整类错误**，比靠文档和自觉强得多。

### 第 4 段：decode（28-38 行）—— 整个文件的防御工事

```cpp
Header decode(const std::uint8_t* p, std::size_t n, std::uint32_t max_payload) {
```

**先看一个 C++ 规则**：`max_payload` 的默认值写在 **hpp 的声明**里，这里定义时**不能重复写**。C++ 规定默认实参在同一作用域只能给一次，写两遍是编译错误。初学者常踩。

参数用"裸指针 + 长度"是 C++17 里传数组的标准姿势（C++20 的 `std::span` 更安全）。

接下来是**六个卫语句（guard clause）**，每个失败就抛异常。这种"先验证、后使用"的扁平结构，是网络协议解析的**唯一正确姿势**——因为输入完全来自不可信的对端。

#### ① `if (n != header_size)` — 整个函数内存安全的基石

```cpp
if (n != header_size) throw std::runtime_error("invalid header size");
```

**注意是 `!=` 而不是 `<`。** 这一行的意义远超"检查一下长度"：

因为确认了 `n == 24`，后面所有 `p + 20`（读 4 字节，占 20-23）、`p + 16`（读 4 字节，占 16-19）、`p + 8`（读 8 字节，占 8-15）**必然落在界内**。这一行立住了，后面五行的内存访问就全都合法。

如果偷懒不检查，这个函数就是一个**任意越界读原语**：调用者传个 5 字节的缓冲区，它会一路读到堆外。改成 `n < header_size` 也是错的——传进来的长度大于 24 说明调用者搞错了（比如把整条消息的指针传进来了），也应该拒绝。

#### ② `if (!std::equal(p, p + 4, "VFLW"))` — magic

```cpp
if (!std::equal(p, p + 4, "VFLW")) throw std::runtime_error("bad magic");
```

`std::equal` 比较两个区间，**不读结尾的 `\0`**（这和 `strcmp` 的差别正是它的价值）。

作用：一眼判断"这是不是 VFLW 数据"。对方发来 HTTP 请求、或者更常见的——**TCP 字节流错位了**（上一次读取少读了几个字节），magic 立刻不匹配。**这是所有检查里最能快速定位问题的**。

讲义特意强调过：**magic 不是密码**，它不证明对方可信，只识别格式。别把它当安全机制。

#### ③ `if (get(p + 4, 2) != 1)` — version

```cpp
if (get(p + 4, 2) != 1) throw std::runtime_error("unsupported version");
```

将来协议升级到 v2 时，老程序看到 `version = 2` 会**明确拒绝**，而不是拿着 v1 的理解去解析 v2 的数据，产生一堆看起来像数据、其实是噪声的结果。**这就是"快速失败"（fail fast）。**

#### ④ `if (get(p + 20, 4) != 0)` — flags

```cpp
if (get(p + 20, 4) != 0) throw std::runtime_error("unsupported flags");
```

**协议设计里最漂亮的一招：前向兼容的保险。**

flags 现在恒为 0，但它是**预留**给未来的（压缩标志、加密标志、分片标志……）。将来新程序使用了某个 flag 发给老程序，老程序看到非 0 就立刻拒绝——而不是忽略 flags、按自己的理解解析出错误结果。

**预留字段 + 严格拒绝非默认值**，这是让自己将来能安全演进的经典手法。反过来，如果这里写成"忽略 flags"，协议就永远无法安全地增加新特性了。

#### ⑤ 类型合法性

```cpp
const auto type = get(p + 6, 2);
if (type < 1 || type > 3) throw std::runtime_error("unknown message type");
```

**注意顺序**：先 `get` 到**裸整数**，用整数比大小，**而不是**先 `static_cast<Type>` 再比较。两个原因（详见问题 4）：

1. `Type` 是 enum class，**根本不能**和 `1`、`3` 比大小
2. 更重要的：`static_cast<Type>(999)` 是**合法**的，枚举不做值域检查。先 cast 再校验等于把校验这道门放在了信任之后

一个隐患：这里的 `1` 和 `3` 是**魔法数字**，必须和 [protocol.hpp:10](../../../include/visionflow/protocol/protocol.hpp#L10) 的枚举保持一致。加一个 `heartbeat = 4`，这一行**一声不吭地**就错了。

#### ⑥ `if (length == 0 || length > max_payload)` — 安全防线

```cpp
const auto length = get(p + 16, 4);
if (length == 0 || length > max_payload) throw std::runtime_error("payload length outside limit");
```

**这是整个文件里安全意识最强的一行。**

`payload_length` 是**攻击者完全可以控制的字段**。对端可以在消息头里写"我接下来有 4GB 的数据"。如果下游代码天真地 `buffer.reserve(length)` 或 `new uint8_t[length]`，服务端当场内存耗尽——**一次不需要任何认证的拒绝服务攻击**。

所以规则是：**永远不要相信对端声称的长度，必须在分配任何资源之前，用自己的上限去验证它。** `max_payload` 就是从外面传进来的"我自己能接受多少"。

（`length == 0` 也拒绝是当前协议的设计决策：消息体不能为空——总得有个 JPEG 或 JSON。这是业务规则，不是安全规则。）

**把这个上限做成参数而不是硬编码，是个好设计**：机制（怎么检查）和策略（上限是多少）分离。默认值 16 MiB 在 hpp 里，不同场景可以传不同的值——[core_tests.cpp:18](../../../tests/unit/core_tests.cpp#L18) 就是这么测的。

顺便：`length` 是 `std::uint64_t`，`max_payload` 是 `std::uint32_t`，比较时前者把后者提升为 64 位——**两个都是无符号，安全**。真正的陷阱是有符号/无符号混合比较，这里没有。

#### 最后一行：返回

```cpp
return {static_cast<Type>(type), get(p + 8, 8), static_cast<std::uint32_t>(length)};
```

花括号是**聚合初始化**，按 `Header` 里成员的**声明顺序**对应：`type`、`request_id`、`payload_length`。顺序必须和 hpp 里一致。

**两个 cast 都不是装饰，是必需的**：

- `static_cast<Type>(type)`：`type` 是整数，`Header::type` 是 `Type`，enum class 不允许隐式转换
- `static_cast<std::uint32_t>(length)`：`length` 是 `uint64_t`，成员是 `uint32_t`。而且**花括号初始化禁止窄化转换**——不写 cast 是**编译错误**（这是 `{}` 比 `()` 安全的地方：`()` 会静默截断，`{}` 会拦下来）

**这里有个漂亮的自洽**：第 36 行的检查保证了 `length ≤ max_payload`（`uint32_t`），所以第 37 行的 `static_cast<uint32_t>` 一定安全。**检查与转换紧紧挨着，转换的安全性由上一行的检查直接担保。** 这是这份代码里最值得学的模式——每次看到 `static_cast`，都该能指着某一行检查说"它保证了这个 cast 是安全的"。

### 第 5 段：packet（39-47 行）

```cpp
std::vector<std::uint8_t> packet(Type type, std::uint64_t id, const std::vector<std::uint8_t>& body) {
```

把消息头和消息体拼成一条完整的消息。参数传递有讲究：`type`/`id` 小且便宜，**按值传**；`body` 可能很大，**按 const 引用传**避免拷贝。

#### 长度上限检查

```cpp
if (body.size() > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("payload too large");
```

**为什么必须有**：下一行要把 `body.size()`（`std::size_t`，64 位）cast 成 `uint32_t`。如果 body 超过 4GB，cast 会**静默截断**——头里声明一个错误的长度，接收方就会读到错位的数据，**而且不会有任何报错**。

三个细节：

- **用 `std::numeric_limits<uint32_t>::max()` 而不是硬编码 `4294967295`**：可读、不会抄错、将来类型改了自动跟着变
- **异常类型是 `std::length_error` 而非 `runtime_error`**：语义更精确。它是 `logic_error` 的子类，表示"长度超出可表示范围"，而不是"运行期意外"。选异常类型就像选变量名，是给读代码的人（和 catch 的人）的信息
- 在 32 位平台上 `size()` 不可能超过 `uint32_t::max()`，这个检查是**死代码**。但为了跨平台正确性必须写——**便宜的保险不叫浪费**

#### 拼装

```cpp
const auto h = encode({type, id, static_cast<std::uint32_t>(body.size())});
std::vector<std::uint8_t> out;
out.reserve(header_size + body.size());
out.insert(out.end(), h.begin(), h.end());
out.insert(out.end(), body.begin(), body.end());
return out;
```

再次注意这个 cast 紧贴着上面的检查——**同一个模式，第二次出现**。

**`reserve` 是最值得学的性能习惯**：`vector` 扩容是摊还 O(1)，但每次扩容都要重新分配 + 拷贝全部元素。这里**已经知道最终大小**（`header_size + body.size()`，精确值），一次分配到位，**零次重分配**。对一条刚好 1MB 的消息，不 reserve 可能要触发十几次重分配、拷贝几十 MB 的数据。

两次 `insert` 必须**先头后体**。`std::array` 和 `std::vector` 的迭代器在这里用法一致——这就是迭代器的价值：不同容器，同一套接口。返回值靠 RVO（返回值优化）/ 移动，**没有拷贝**。

### 全貌：三个函数串起来

```text
                       ┌──── encode ────┐
   Header (内存)  ────► │  24 个协议字节  │ ──┐
                       └────────────────┘   │
                                            ├──► packet ──► 一条完整消息
   body   (内存)  ───────────────────────────┘                  (内存)
                                                                  │
                                                                  ▼
                                                           TCP / 文件
                                                                  │
                       ┌──── decode ────┐                          │
   Header (内存)  ◄──── │  六个检查通过  │ ◄── 切片出前 24 字节 ◄────┘
                       └────────────────┘
```

`encode` 和 `decode` **互为逆运算**，这个性质可以用往返测试（round-trip）来验证。

### 顺带：测试覆盖了什么

[core_tests.cpp](../../../tests/unit/core_tests.cpp) 测得很不错：

| 测试 | 覆盖的检查 |
| --- | --- |
| [`:14`](../../../tests/unit/core_tests.cpp#L14) `h[8]==1 && h[15]==8 && h[16]==0 && h[17]==1` | 大端序真实生效（不只是"能读回来"） |
| [`:15`](../../../tests/unit/core_tests.cpp#L15) `decode(h.data(), h.size(), ...)` | 正常路径 |
| [`:17`](../../../tests/unit/core_tests.cpp#L17) `decode(h.data(), 23, ...)` | ① 长度不是 24 |
| [`:19-21`](../../../tests/unit/core_tests.cpp#L19-L21) `h[0]=255` | ② magic |
| [`:19-21`](../../../tests/unit/core_tests.cpp#L19-L21) `h[5]=255` | ③ version |
| [`:19-21`](../../../tests/unit/core_tests.cpp#L19-L21) `h[7]=255` | ⑤ type 越界 |
| [`:19-21`](../../../tests/unit/core_tests.cpp#L19-L21) `h[23]=255` | ④ flags |
| [`:23-24`](../../../tests/unit/core_tests.cpp#L23-L24) `payload_length = 0` | ⑥ 空载荷 |
| [`:18`](../../../tests/unit/core_tests.cpp#L18) `max_payload = 65535` | ⑥ 超上限 |

**六个检查全都有对应的负面测试**，而且第 19 行那个 `{0, 5, 7, 23}` 是精心挑的——每个偏移正好打中一个不同的检查。

**但有两个缺口值得补**：

1. **`type` 没有做往返验证**。[第 16 行](../../../tests/unit/core_tests.cpp#L16)只检查了 `request_id` 和 `payload_length`：
   ```cpp
   check(decoded.request_id == 0x0102030405060708ULL && decoded.payload_length == 65536, "header round trip");
   ```
   没有 `decoded.type == protocol::Type::image`。现在 type 只被"值域在 1..3 内"间接验证了——万一编码时写错字节（比如写成了 `result`），这个测试**发现不了**。加一句就补上了。

2. **`packet()` 完全没有测试**。它的拼装正确性、`reserve` 的精确性、`throws length_error` 的分支，一行都没覆盖。而拼接顺序错了（先 body 后 header）是**测不出来的**。

好消息是：**核心安全属性已经被测住了**，剩下的都是覆盖率补全，不是设计缺陷。

> **一句话总结这份文件的设计哲学**：所有"外部数据"和"内部数据"的交界处，都有一道显式的检查门，每次类型转换都紧挨着担保它的那次检查。`decode` 是入口门，`packet` 是出口门，`put`/`get` 是门两边的搬运工，而"门在哪"本身被 `static_cast` 标记得一清二楚——`grep static_cast` 就能审完全部的信任边界。

---

## 附：本课关键代码与术语速查

### 代码

| 位置 | 代码 | 作用 |
| --- | --- | --- |
| [protocol.hpp:8](../../../include/visionflow/protocol/protocol.hpp#L8) | `namespace visionflow::protocol {` | C++17 嵌套命名空间；声明与定义靠"重开"同一个 namespace 对应 |
| [protocol.hpp:9](../../../include/visionflow/protocol/protocol.hpp#L9) | `constexpr std::size_t header_size = 24;` | 编译期常量；命名空间作用域默认**内部链接** |
| [protocol.hpp:10](../../../include/visionflow/protocol/protocol.hpp#L10) | `enum class Type : std::uint16_t {...}` | 有作用域枚举 + 指定 2 字节底层类型 |
| [protocol.hpp:16](../../../include/visionflow/protocol/protocol.hpp#L16) | `std::array<std::uint8_t, header_size>` | 常量作模板参数；`24` 成为**类型的一部分** |
| [protocol.cpp:6-18](../../../src/protocol/protocol.cpp#L6-L18) | 匿名 `namespace { put, get }` | 内部链接的实现细节，跨 TU 不撞名 |
| [protocol.cpp:7-12](../../../src/protocol/protocol.cpp#L7-L12) | `put()` 倒着填最低位 | 大端序写入；**不检查 count 是否装得下 n** |
| [protocol.cpp:13-17](../../../src/protocol/protocol.cpp#L13-L17) | `get()` 正着攒高位 | 与 `put` 对称的大端序读取 |
| [protocol.cpp:20](../../../src/protocol/protocol.cpp#L20) | `std::array<uint8_t, header_size> out{};` | 值初始化清零 24 字节 → flags 恒为 0；**漏了它就是内存泄漏漏洞** |
| [protocol.cpp:22](../../../src/protocol/protocol.cpp#L22) | `put(out.data() + 4, 1, 2)` | version 硬编码 1：固定字段不进 `Header`，让错误构造不出来 |
| [protocol.cpp:29](../../../src/protocol/protocol.cpp#L29) | `if (n != header_size)` | **内存安全基石**：确认恰好 24 字节，后五行越界读全部合法 |
| [protocol.cpp:30](../../../src/protocol/protocol.cpp#L30) | `std::equal(p, p + 4, "VFLW")` | 不读结尾 `\0`；快速识别格式错位 |
| [protocol.cpp:32](../../../src/protocol/protocol.cpp#L32) | `if (get(p + 20, 4) != 0)` | 预留字段 + 拒绝非默认值 = 前向兼容保险 |
| [protocol.cpp:33-34](../../../src/protocol/protocol.cpp#L33-L34) | 先取裸整数再校验范围 | 枚举**不做值域检查**，`static_cast<Type>(999)` 合法 |
| [protocol.cpp:35-36](../../../src/protocol/protocol.cpp#L35-L36) | `if (length == 0 \|\| length > max_payload)` | **安全防线**：永不信任对端声称的长度 |
| [protocol.cpp:37](../../../src/protocol/protocol.cpp#L37) | 两个 `static_cast` | `{}` 禁止窄化转换，cast 是必需的；安全性由上一行检查担保 |
| [protocol.cpp:40](../../../src/protocol/protocol.cpp#L40) | `std::length_error("payload too large")` | 语义精确的异常类型；`logic_error` 子类 |
| [protocol.cpp:43](../../../src/protocol/protocol.cpp#L43) | `out.reserve(header_size + body.size())` | 已知最终大小 → 零次重分配 |

### 术语

| 术语 | 含义 |
| --- | --- |
| namespace | 名字的**编译期**前缀，零运行时开销；可反复打开，和 class 不同 |
| 匿名 namespace | 无名的 namespace，成员具**内部链接**；C 里 `static` 的加强版 |
| `using` 声明 / 指令 | 前者引入单个名字，后者引入整个 namespace；**头文件里都别用** |
| ADL（Koenig 查找） | 按实参所属 namespace 查找函数；`std::cout << x` 依赖它 |
| mangled name | 链接器看到的真名，**包含 namespace**；`nm` 里 `T` 外部 / `t` 内部 |
| `const` | 值**不能改**（只读），不承诺何时算出来 |
| `constexpr` | 值**编译期已知**；变量隐含 `const`，命名空间作用域默认内部链接 |
| `if constexpr` | C++17；模板中不成立的分支被**丢弃**，无需合法（非模板中仍须合法） |
| `consteval` | C++20；**必须**编译期求值，是 `constexpr` 的加强版 |
| `enum class` | 有作用域枚举；不隐式转整数、不跨类型比较；可指定底层类型 |
| 底层类型 | `enum class Type : std::uint16_t` 中的 `std::uint16_t`；决定 `sizeof` |
| 类型安全 ≠ 值域安全 | 枚举只保证"标签类型对"，`static_cast<Type>(999)` 仍合法，范围检查不能省 |
| 大端序（network byte order） | 高位字节在前；`put` 倒着填、`get` 正着攒 |
| 卫语句（guard clause） | 逐个检查、失败即抛的扁平结构；协议解析的标准姿势 |
| 快速失败（fail fast） | 宁可当场拒绝，也不要静默算错；version/flags 检查的意义 |
| `reserve` | 已知最终大小时一次性分配，避免 vector 反复扩容拷贝 |
| RVO | 返回值优化；返回容器/数组**不产生拷贝** |
| 聚合初始化 | `{a, b, c}` 按成员**声明顺序**填入；禁止窄化转换（比 `()` 安全） |
| 信任边界 | "未验证的外部数据"与"可信的强类型值"的转换点，由 `static_cast` 标出 |
