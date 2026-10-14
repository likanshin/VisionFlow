# 第五模块：JSON 结果输出

## 1. 本课目标与分工

熟悉程度已确认：学生要求从头讲解 JSON。本讲义把基本结构、序列化、项目内的代码和文件保存集中讲完，不再把每一段操作拆成独立课程。

完成后，在已有图片输出之外，程序还会把处理器名称、图片尺寸、边缘数量和最终线段写入 `build/result.json`。先使用固定输出路径，延续现有图片保存方式。

当前进度：2026-09-25，学生已完成 JSON 输出代码，并提供配置、构建及两种处理器的成功运行记录。CMake 找到 nlohmann/json 3.7.3；edges 的 result.json 包含正确尺寸、2095 个边缘像素和空 lines 数组；lane_lines 的 lines 包含 [82,358,287,203]、[352,202,556,356]、[355,205,558,358] 三条线段，processor 字段对应 lane_lines。老师只读核对了对象构造、嵌套数组、dump(2)、文件打开及关闭后的状态检查，未代为配置、构建、运行或测试。学生正确区分数字和字符串，说明 dump(2) 的缩进含义并指出 output << json_text 负责写入；老师补充 dump 将 JSON 对象序列化为字符串。JSON 理解检查通过，本知识点已完成。下一步是配置读取与校验，先询问熟悉程度，尚未开始实现。命令行课程已完成，不重做之前已掌握或明确跳过的练习。

## 2. 为什么需要 JSON

现在程序会输出：

```text
Width: 640
Height: 360
Edge pixels: 2095
```

人读起来很直接。如果将来 Python 客户端要使用结果，就得自己约定如何拆分这些行，如何识别字段、数字和线段列表。

JSON 提供一种通用的数据表示格式。它以文本记录有结构的数据，不要求读它的程序也使用 C++。例如，下面是一份缩小的结果示例：

```json
{
  "processor": "edges",
  "width": 640,
  "height": 360,
  "edge_pixels": 2095,
  "lines": []
}
```

这里包含的是数据，没有“执行处理器”的命令。另一个程序解析后，可以按 width 这个字段名找到对应的数字。

JSON 只是数据格式：它不负责处理图片，也不自动保存文件或通过网络发送。本课把 JSON 保存到文件，后续网络模块再研究传输。

## 3. 先看懂 JSON 的结构

### 对象：用名称描述一组数据

最外面的 `{}` 表示对象，其中每一项是“名称：值”：

```json
{"width": 640, "height": 360}
```

width、height 是字段名，也常叫键；640、360 是对应的值。字段名使用双引号，冒号分开键和值，逗号分开相邻字段。JSON 对象和 C++ 类实例不是同一个概念；这里的对象是一组键值对。

### 数组：按顺序装一组数据

`[]` 表示数组。例如一条线段可按项目约定写成：

```json
[82, 358, 287, 203]
```

四个位置依次表示 x1、y1、x2、y2。JSON 本身不知道它是线段，这是我们规定的含义。多条线段再放进一个外层数组：

```json
{
  "lines": [
    [82, 358, 287, 203],
    [352, 202, 556, 356]
  ]
}
```

`"lines": []` 表示字段存在、值是空数组，即当前没有最终线段；`"lines": null` 则是空值，不能当成空数组。本课约定 lines 始终为数组。

### 值有类型

| 类型 | 示例 | 含义 |
| --- | --- | --- |
| 字符串 | "edges"、"640" | 文本 |
| 数字 | 640、0.35 | 数值 |
| 布尔值 | true、false | 真或假 |
| 空值 | null | 空值；具体业务含义由程序约定 |
| 对象 | {"width": 640} | 一组键值对 |
| 数组 | [82, 358, 287, 203] | 有顺序的一组值 |

`640` 和 `"640"` 不同：前者是数字，后者是字符串。对象的键值对应关系才有意义，不应依赖键的排列顺序；数组则依赖元素顺序。

标准 JSON 不支持注释，最后一项后也不能多写逗号。字符串和字段名用双引号；true、false、null 使用小写，不加引号。这些基本结构见 [JSON 标准 RFC 8259](https://www.rfc-editor.org/rfc/rfc8259.html#section-3)。

## 4. 序列化究竟做什么

当前 `ProcessResult result` 是程序内存中的 C++ 数据。保存成文件涉及三个不同阶段：

```text
ProcessResult 的字段
        ↓ 挑选字段并构造 JSON 对象
内存中的 nlohmann::json 对象
        ↓ dump(2)：序列化
std::string 中的 JSON 文本
        ↓ std::ofstream：写入文件
build/result.json
```

序列化是将内存中的数据转换成可保存或传输的表示。反过来，把 JSON 文本读回结构化数据，称为解析或反序列化；后续配置文件课程会实践读取。

原项目使用 nlohmann/json，本课沿用它。C++17 标准库本身不提供这个 JSON 类型，它来自第三方库；文件输出流 std::ofstream 则来自 C++ 标准库。

不要自己拼接带引号和逗号的 JSON 文本。让库负责格式和字符串转义，能避免漏引号、漏逗号等问题。

这三行展示库中的对象操作，先阅读，不要单独插入程序：

```cpp
nlohmann::json document = nlohmann::json::object();
document["width"] = 640;
const std::string text = document.dump(2);
```

第一行创建内存中的空对象；第二行设置一个数字字段；第三行得到字符串。`dump(2)` 的 2 表示每层缩进两个空格，并非保留两位小数。`dump()` 默认使用紧凑格式；两种格式包含相同数据。`dump` 本身不创建文件。参见 [dump 官方文档](https://json.nlohmann.me/api/basic_json/dump/)。

## 5. 结果字段与依赖配置

### 本课导出什么

| JSON 字段 | C++ 来源 | 用途 |
| --- | --- | --- |
| status | 固定字符串 "ok" | 标识成功产生的结果 |
| processor | processor_name | 实际选择的处理器 |
| width、height、channels | result 中对应字段 | 图片尺寸与通道数 |
| edge_pixels | result.edge_pixels | 全图边缘像素数量 |
| lines | result.lines | 每条线段转为 [x1, y1, x2, y2] |

本课输出处理结果摘要。result 中的 cv::Mat 是图片像素数据，继续由原有 imwrite 保存，不直接放进 JSON；ROI 的中间统计仍可在终端观察。本课也不添加尚未学习的请求编号或耗时统计。

### 老师已维护的 CMake

本机只读检查发现已安装 nlohmann/json 3.7.3 的头文件和 CMake 配置；无需本课提前安装依赖。老师已在 `/home/rikanshin/VisionFlow/VisionFlow_learn/CMakeLists.txt` 中添加：

```cmake
find_package(nlohmann_json 3.2 REQUIRED CONFIG)
```

目标依赖改为：

```cmake
target_link_libraries(vision_cli PRIVATE
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
)
```

3.2 是要求的最低版本，不是强制安装 3.2。该库主要通过头文件提供实现；使用它的 CMake 目标可以传递头文件路径等使用要求，并不意味着这里一定有额外的动态库要链接。此接入方式见 [官方 CMake 文档](https://json.nlohmann.me/integration/cmake/)。

以上配置已写入辅助文件，你不必再添加一次。老师没有执行 CMake；实际配置与构建结果以你运行后反馈为准。

## 6. 代码具体写在哪里

只需由你修改 `/home/rikanshin/VisionFlow/VisionFlow_learn/apps/vision_cli.cpp`。先读懂前面的结构和数据流，再按本节写入。

### 6.1 在文件顶部添加头文件

找到已有的：

```cpp
#include <stdexcept>
```

在它下面添加：

```cpp
#include <fstream>
#include <nlohmann/json.hpp>
```

已有的 `<string>` 保留。无需给 ProcessResult 增加新成员，也无需改处理器实现。

### 6.2 在成功保存预览之后插入 JSON 输出

在 main 的 try 末尾找到这两行：

```cpp
        std::cout << "Saved: build/preview.png\n";
        return 0;
```

在两行之间插入下面整段代码，即放在成功输出预览保存消息之后、正常退出之前：

```cpp
        nlohmann::json document = nlohmann::json::object();
        document["status"] = "ok";
        document["processor"] = processor_name;
        document["width"] = result.width;
        document["height"] = result.height;
        document["channels"] = result.channels;
        document["edge_pixels"] = result.edge_pixels;
        document["lines"] = nlohmann::json::array();

        for (const auto& segment : result.lines) {
            document["lines"].push_back(nlohmann::json::array({
                segment[0], segment[1], segment[2], segment[3]
            }));
        }

        const std::string json_text = document.dump(2);
        const std::string output_path = "build/result.json";
        std::ofstream output(output_path);
        if (!output) {
            throw std::runtime_error("cannot open output: " + output_path);
        }

        output << json_text << '\n';
        output.close();
        if (!output) {
            throw std::runtime_error("cannot write output: " + output_path);
        }

        std::cout << "Saved: " << output_path << "\n";
```

正常退出的 return 0 和后面的两个 catch 都保留。将代码放在 try 内，写文件错误就能由已有的 std::exception catch 报告并返回 1。

### 6.3 逐段理解

**构造对象。** `document["width"] = result.width` 复制的是字段值，不是字段地址。库知道右侧是整数，因此生成 JSON 数字，不能为了“变成文本”先把它转成字符串。

**构造数组。** `nlohmann::json::array()` 明确创建空数组，所以没有线段时也会输出 `[]`。每次循环把四个坐标构成一个内层数组，再追加到外层 lines 数组末尾。array 的用法见 [官方文档](https://json.nlohmann.me/api/basic_json/array/)，追加操作见 [push_back 文档](https://json.nlohmann.me/api/basic_json/push_back/)。

**序列化。** `json_text` 是转换后的 JSON 文本，与前面的 document 对象不同；仅调用 dump 并不会写文件。

**打开文件。** `std::ofstream output(output_path)` 创建文件输出流。默认会创建文件；如果目标已存在，则覆盖原内容。它不会自动创建父目录，所以这里使用已有的 build 目录。`if (!output)` 检查打开是否成功。

**写入并确认。** `output << json_text << '\n'` 把文本写入流。随后显式 close，关闭时会处理缓冲数据，再检查流状态，以便报告写入或关闭时发生的错误。局部输出流仍遵循 RAII；这里提前 close 是为了在返回成功前检查关闭结果。

**成功位置。** 只有前面的处理和图片保存成功，才会走到 JSON 输出。帮助分支会在更早处 return，不会创建 JSON 文件。失败时磁盘上的旧 result.json 可能仍在，因此不能把“文件存在”当作本次成功的证据。

这里先将转换逻辑写在 main，便于看清整个过程；后续需要复用时可以提取普通函数或成员函数。两种做法都可以，本课不再同时引入新的文件拆分任务。

## 7. 你自行配置、构建和观察

下列命令由学生执行，老师未执行。CMake 增加了依赖，所以先重新配置现有 build 目录：

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake -S . -B build
cmake --build build
./build/vision_cli --input samples/frame.jpg --processor edges
echo $?
cat build/result.json
```

每一步成功后再执行下一步。如果报错，把错误发给老师分析。

在当前样例和参数下，edges 的预期 JSON 内容如下。字段排列可能不同，键和值应一致；这是一份预期示例，不是老师运行得到的输出：

```json
{
  "status": "ok",
  "processor": "edges",
  "width": 640,
  "height": 360,
  "channels": 3,
  "edge_pixels": 2095,
  "lines": []
}
```

随后可切换到 lane_lines，观察 lines 从空数组变为包含坐标数组的列表：

```bash
./build/vision_cli --input samples/frame.jpg --processor lane_lines
cat build/result.json
```

每次运行都覆盖同一个 result.json。当前样例的最终线段数量预期为 3；具体坐标以你的运行结果为准。

终端上仍有欢迎、统计和保存提示，因此整个终端输出不是一份纯 JSON 文档。供其他程序读取的是 result.json 文件。

## 8. 完成本知识点后的理解检查

完成后可以发来 result.json 的内容，并解释下面两点；不用每写完一个小节就停下来：

1. `document["width"] = 640` 和 `document["width"] = "640"` 的 JSON 有什么区别？
2. `document.dump(2)` 做了什么，文件真正在哪一步被写入？

如果前面的对象、数组或序列化还不清楚，可以直接指出不理解的句子，我们在本讲义中补充说明。JSON 读取与配置校验属于下一知识点，届时再确认熟悉程度。
