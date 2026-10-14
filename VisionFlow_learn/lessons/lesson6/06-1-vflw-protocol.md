# 第六模块：VFLW 消息格式与字节编解码

## 1. 从你已经知道的概念出发

你已了解字节、定长整数和大小端，但还不了解 VFLW。本课不重讲这些基础，集中讲清：协议在约定什么、为什么需要消息头、怎样把字段编码成字节并解码回来。

VFLW 是本项目自行定义的应用层消息格式。它的名字来自消息开头的四个 ASCII 字节 `VFLW`。学会它不需要安装一个名为 VFLW 的库，需要让发送方和接收方遵守相同的格式。

主要代码由你亲手编写。本课先处理内存中的字节数组，不连接网络、不改现有图像处理 CLI。下面的实现和验证方法集中放在这一份讲义中；先读懂格式，再动手，不必读完一小节就停下来回答问题。

当前状态：学生已完成实现，提供成功配置、构建和运行记录，退出码为 0；固定头部字节比对、字段解码、消息拼接及长度超限拒绝检查通过。学生正确回答总长 1024，并解释三个可变字段与三个固定字段的区别，理解检查通过。老师只读核对源码，未配置、构建、运行或测试本课；未演示的非法头部分支不记作已验证。下一步模块 7，先确认 TCP/socket 熟悉程度。

## 2. 为什么需要约定格式

现在 `vision_cli` 直接从本地文件读取图片，所以程序知道从哪个文件取数据。以后 Python 要把图片发给 C++ 服务端，服务端需要知道：

- 当前收到的是图片请求、处理结果，还是错误信息？
- 这份结果对应哪一次请求？
- 当前消息的内容有多少字节，在哪里结束？

TCP 提供字节流，一次发送的数据不保证恰好由一次接收取得。连续两张图片的数据可能连续到达，也可能被拆成许多小段。因此，我们为每份内容加上一段有固定格式的说明，这就是消息头。

```text
一条 VFLW 消息
┌───────────────┬───────────────────────────┐
│ 消息头：24 字节 │ 消息体：payload_length 字节 │
└───────────────┴───────────────────────────┘
```

消息体又叫 payload（载荷），就是这条消息实际携带的内容。图片请求携带 JPEG 文件的压缩字节，不是解码后的 `cv::Mat` 内存，也不是图片路径；结果和错误消息携带 UTF-8 编码的 JSON 文本。

VFLW 负责格式，TCP 负责传输。给字节加消息头本身既不发送数据，也不压缩或加密数据。

## 3. 本项目的真实格式

依据原项目的 [协议文档](../../../docs/protocol.md)、[头文件](../../../include/visionflow/protocol/protocol.hpp) 和 [实现](../../../src/protocol/protocol.cpp)。多字节整数统一使用大端序。

偏移是相对于消息起点的字节下标，从 0 开始。

| 偏移 | 字节数 | 字段 | 含义 |
| --- | --- | --- | --- |
| 0 | 4 | magic | 固定的 ASCII `VFLW`，用于识别消息格式 |
| 4 | 2 | version | 当前协议版本，固定为 1 |
| 6 | 2 | type | 1：JPEG 请求；2：JSON 结果；3：JSON 错误 |
| 8 | 8 | request_id | 无符号请求编号，用于把响应和请求对应起来 |
| 16 | 4 | payload_length | 消息体字节数，不包含 24 字节消息头 |
| 20 | 4 | flags | 当前固定为 0，保留给将来的扩展 |

合计 `4 + 2 + 2 + 8 + 4 + 4 = 24` 字节。

`magic` 是格式标记，不是密码，也不能证明对方可信。`version` 让程序拒绝自己不支持的格式。`request_id` 让客户端能判断“这份结果属于哪张图片”，以后不能假设响应一定按发送顺序返回。当前 flags 必须为 0，不能随意填写。

例如：第 42 号图片请求，假设消息体长度为 5 字节，头部为：

```text
56 46 4C 57 | 00 01 | 00 01 | 00 00 00 00 00 00 00 2A | 00 00 00 05 | 00 00 00 00
   VFLW      版本1    类型1            请求编号42            长度5       标志0
```

这里的 5 只是便于手算的示例，不代表 5 字节足以组成有效 JPEG。一条消息的总长度是 `24 + 5 = 29` 字节。接收方先收齐 24 字节，再读取头中指定的 5 字节消息体，之后才开始下一条消息。

注意数字和文本的区别：长度 5 写成四个字节 `00 00 00 05`，不是写入字符 `'5'`。而消息体中的 JSON 本来就是文本，会按文本编码保存。

## 4. 为什么不直接发送 struct

我们会用 `Header` 结构体在 C++ 内保存字段。但内存中的结构体不等于协议字节：成员之间可能有填充，整数在本机内存中的字节序也可能与协议不同。

因此不能拿 `sizeof(Header)` 当消息头长度，也不能把结构体内存直接复制到网络。要明确地把每个字段写到规定的偏移上。

```text
Header 字段 → encode → 24 个协议字节
24 个协议字节 → decode → 检查合法性 → Header 字段
```

本课接口沿用原项目的思路：结构体只保存变化的 type、request_id、payload_length。固定的 magic、version 和 flags 由编码函数写入，解码函数检查。

## 5. 需要写在哪些文件

在 `/home/rikanshin/VisionFlow/VisionFlow_learn` 中新增以下三个文件。不要修改原项目的同名协议文件。

| 文件 | 用途 |
| --- | --- |
| `include/protocol.hpp` | 声明消息类型、头部字段和编解码函数 |
| `src/protocol.cpp` | 实现大端读写、头部编码解码和消息拼接 |
| `apps/protocol_demo.cpp` | 独立观察及检查协议，不依赖 OpenCV |

现有 `apps/vision_cli.cpp` 保持当前功能。本课用独立入口，避免把消息格式练习混进图片算法中。

### 5.1 新建 include/protocol.hpp，写入完整内容

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace visionflow::protocol {

constexpr std::size_t header_size = 24;
constexpr std::uint32_t default_max_payload = 16U * 1024U * 1024U;

enum class Type : std::uint16_t {
    image = 1,
    result = 2,
    error = 3
};

struct Header {
    Type type = Type::image;
    std::uint64_t request_id = 0;
    std::uint32_t payload_length = 0;
};

std::array<std::uint8_t, header_size> encode(const Header& header);
Header decode(const std::uint8_t* bytes, std::size_t size,
              std::uint32_t max_payload = default_max_payload);
std::vector<std::uint8_t> packet(Type type, std::uint64_t request_id,
                               const std::vector<std::uint8_t>& body);

} // namespace visionflow::protocol
```

`Type` 是有作用域的枚举，使用 `Type::image` 这样的名字，比在代码里到处写 1、2、3 清楚。底层整数类型指定为 16 位，但仍需要显式编码，不能因此直接发送它的内存。

`std::array<uint8_t, 24>` 的元素数固定，适合消息头；`std::vector<uint8_t>` 的长度可变，适合整条消息。`namespace` 把协议相关名字放在一起，避免与其他模块的名字混淆。

`16 MiB` 等于 `16 × 1024 × 1024` 字节。它是本课解码默认采用的消息体限制，不是消息头的一部分。原项目还对服务端响应设置了更小的总长度限制，方向相关的限制留到网络层接入时处理。

### 5.2 新建 src/protocol.cpp，写入完整内容

```cpp
#include "protocol.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace visionflow::protocol {
namespace {

// 从末尾向前填充，低 8 位写到靠后的位置，得到大端字节序。
void put(std::uint8_t* out, std::uint64_t number, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        out[count - i - 1] = static_cast<std::uint8_t>(number & 0xFFU);
        number >>= 8U;
    }
}

// 从高位字节开始，每读入一个字节，就把已有数字左移 8 位。
std::uint64_t get(const std::uint8_t* bytes, std::size_t count) {
    std::uint64_t number = 0;
    for (std::size_t i = 0; i < count; ++i) {
        number = (number << 8U) | bytes[i];
    }
    return number;
}

} // namespace

std::array<std::uint8_t, header_size> encode(const Header& header) {
    std::array<std::uint8_t, header_size> bytes{};
    std::copy_n("VFLW", 4, bytes.begin());
    put(bytes.data() + 4, 1, 2);
    put(bytes.data() + 6, static_cast<std::uint16_t>(header.type), 2);
    put(bytes.data() + 8, header.request_id, 8);
    put(bytes.data() + 16, header.payload_length, 4);
    // 数组已全部初始化为 0，偏移 20 到 23 的 flags 保持为 0。
    return bytes;
}

Header decode(const std::uint8_t* bytes, std::size_t size,
              std::uint32_t max_payload) {
    if (size != header_size || bytes == nullptr) {
        throw std::runtime_error("消息头必须为 24 字节，且数据不能为空");
    }
    if (!std::equal(bytes, bytes + 4, "VFLW")) {
        throw std::runtime_error("消息头标记必须为 VFLW");
    }
    if (get(bytes + 4, 2) != 1) {
        throw std::runtime_error("不支持此协议版本");
    }
    if (get(bytes + 20, 4) != 0) {
        throw std::runtime_error("当前协议的 flags 必须为 0");
    }
    const auto type = get(bytes + 6, 2);
    if (type < 1 || type > 3) {
        throw std::runtime_error("未知的消息类型");
    }
    const auto length = get(bytes + 16, 4);
    if (length == 0 || length > max_payload) {
        throw std::runtime_error("消息体长度必须大于 0，且不能超过允许上限");
    }
    return {static_cast<Type>(type), get(bytes + 8, 8),
            static_cast<std::uint32_t>(length)};
}

std::vector<std::uint8_t> packet(Type type, std::uint64_t request_id,
                               const std::vector<std::uint8_t>& body) {
    if (body.empty() || body.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("消息体不能为空，且长度不能超过 32 位无符号整数的范围");
    }
    const auto header = encode({type, request_id,
                               static_cast<std::uint32_t>(body.size())});
    std::vector<std::uint8_t> bytes;
    bytes.reserve(header_size + body.size());
    bytes.insert(bytes.end(), header.begin(), header.end());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

} // namespace visionflow::protocol
```

这里有几处要结合代码理解：

- `bytes.data() + 16` 指向偏移 16 的字节，`put(..., 4)` 只写入该位置开始的四个字节。
- `put` 和 `get` 在匿名命名空间里，只供当前源文件使用。调用处的 count 为 2、4、8，均没有超过承载数字的 64 位类型。
- `put` 用掩码取最低 8 位，再右移；`get` 左移已有数字，再拼入新字节。例如两个字节 `01 02` 还原为 258。
- `"VFLW"` 作为 C++ 字符串字面量含末尾空字符，但这里只复制前四个字节，协议中没有第五个空字符。
- `decode` 先检查长度，再读取字段；调用者必须提供实际有效的缓冲区。它只解码消息头，不读取消息体，也不能证明 JPEG 或 JSON 内容有效。
- `encode` 沿用原项目职责，只负责编码字段，不验证合法性。`decode` 是接收数据时的校验关口；不要把“能编码”误认为“对方必然接受”。
- `packet` 将头和体拼起来，不执行发送。和原项目相比，这份教学版本提前拒绝空消息体；格式仍兼容。它检查 32 位长度字段能否容纳 body，具体接收上限仍由 decode 的 max_payload 控制。

为什么在按长度分配内存前校验？头中若声称后面有几十亿字节，而程序立刻按这个数字分配，会造成不合理的资源占用。先检查允许的上限，之后才准备接收消息体。

`reserve` 只预留容量，不增加元素数；两次 `insert` 才真正追加了 24 个头部字节和后面的消息体。

### 5.3 新建 apps/protocol_demo.cpp，写入完整内容

这个入口同时做三个有意义的检查：编码是否符合约定的字节、拼接是否保留消息体、非法长度是否会被拒绝。只做“自己编码再自己解码”不足以检查字节序，因为两个函数可能犯同样的错误。

```cpp
#include "protocol.hpp"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace protocol = visionflow::protocol;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    try {
        const protocol::Header original{protocol::Type::image, 42, 5};
        const auto bytes = protocol::encode(original);
        const std::array<std::uint8_t, protocol::header_size> expected{
            0x56, 0x46, 0x4C, 0x57, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,
            0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00
        };
        require(bytes == expected, "编码结果与协议规定的字节不一致");
        const auto decoded = protocol::decode(expected.data(), expected.size());
        require(decoded.type == protocol::Type::image &&
                decoded.request_id == 42 && decoded.payload_length == 5,
                "解码得到的字段不正确");

        std::cout << "消息头字节：";
        for (const auto byte : bytes) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<unsigned int>(byte) << ' ';
        }
        std::cout << std::dec << '\n';
        std::cout << "请求编号：" << decoded.request_id << '\n';
        std::cout << "消息体长度：" << decoded.payload_length << '\n';

        // JSON 消息体就是文本的字节；本例只有 ASCII 字符，也是合法 UTF-8。
        const std::string text = R"({"status":"ok"})";
        const std::vector<std::uint8_t> body(text.begin(), text.end());
        const auto message = protocol::packet(protocol::Type::result, 42, body);
        const auto result_header = protocol::decode(message.data(), protocol::header_size);
        require(result_header.type == protocol::Type::result &&
                result_header.request_id == 42 &&
                result_header.payload_length == body.size(), "拼接后的消息头不正确");
        require(message.size() == protocol::header_size + body.size(),
                "整条消息的字节数不正确");
        require(std::equal(body.begin(), body.end(),
                           message.begin() + protocol::header_size),
                "拼接改变了消息体内容");
        std::cout << "整条结果消息长度：" << message.size() << '\n';

        bool rejected = false;
        try {
            // 头中声明长度为 5，但本次仅允许最多 4 字节。
            (void)protocol::decode(expected.data(), expected.size(), 4);
        } catch (const std::runtime_error& error) {
            rejected = true;
            std::cout << "已拒绝超限消息：" << error.what() << '\n';
        }
        require(rejected, "解码器没有拒绝超限消息");
        std::cout << "协议检查通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "协议检查失败：" << error.what() << '\n';
        return 1;
    }
}
```

输出字节前转成 `unsigned int`，是为了按数字显示；直接输出 `uint8_t` 在常见平台上可能按字符显示。`std::hex` 切换到十六进制，`setw(2)` 与 `setfill('0')` 使每个字节显示成两位；循环结束后恢复 `std::dec`。

这里的 `require` 是很小的检查函数，失败就抛出异常，没有引入新的测试框架。这个演示中的结果 JSON 只用于展示消息拼接，未实现原项目完整的业务结果结构和 JSON request_id 对应检查；后者属于网络业务接入。

### 5.4 配套 CMake 内容与运行方法

三个源码文件写完后，需要在学习工程 `CMakeLists.txt` 的末尾、现有 `target_link_libraries(vision_cli ...)` 块之后添加以下内容。这是老师提供的配套构建配置，与原有 vision_cli 目标并列：

```cmake
add_executable(protocol_demo
    apps/protocol_demo.cpp
    src/protocol.cpp
)
target_include_directories(protocol_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
```

协议演示只依赖标准库，不必给它链接 OpenCV 或 nlohmann/json。当前顶层 CMake 仍然会为原有 vision_cli 查找这些依赖。

完成上述文件后，由你在 `/home/rikanshin/VisionFlow/VisionFlow_learn` 运行：

```bash
cmake -S . -B build
cmake --build build --target protocol_demo
./build/protocol_demo
echo $?
```

根据代码推导，正常输出应包括请求编号 42、消息体长度 5、结果消息总长度 39（24 字节头加 15 字节 JSON 文本），随后显示已拒绝超限消息和“协议检查通过”，退出码为 0。这里是预期结果，不是老师实际运行的记录。

## 6. 本课边界和理解检查

你完成的是字段到字节的编码、字节到字段的解码、头和体的拼接，以及接收头部时的合法性检查。网络模块接下来才负责收齐头和体、处理部分收发，再把图片体交给 OpenCV。

`decode` 的 size 参数要求恰好 24，因此完整消息传给它时要指定头部长度，而不是传整条消息的长度。也不要根据一次 socket 接收返回的字节数就判断一条消息是否完整。

完成整份实践后集中思考这两个问题，不需要每写几行就停下检查：

1. 若 payload_length 是 1000，整条消息共多少字节？接收方收齐消息头后，还需要取得多少字节？
2. 为什么本机的 Header 结构体可以只保留三个字段，但编码结果必须包含格式表中的全部六项？

下一课进入 TCP 前再确认该知识点理解情况。当前不重复已完成的命令行、JSON、配置和日志课程。
