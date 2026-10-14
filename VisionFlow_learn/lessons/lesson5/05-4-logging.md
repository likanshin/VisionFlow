# 第五模块：日志

## 1. 本课目标与进度

学生已完成命令行、JSON 输出及配置读取与校验，要求从头讲解日志。本讲义集中讲清日志用途、等级、时间、上下文、输出位置和项目内的完整接入。

完成后，CLI 会记录开始、完成或错误事件；配置 log_level 控制是否显示普通运行日志。每条日志都是单行 JSON，写入标准错误流；图片和 build/result.json 继续按原有方式保存。

当前状态：学生已实现 Logger、日志等级配置及统一错误记录，并提供 info 模式的 started/completed 日志和 unknown 处理器的 error 日志；错误上下文为 stage=create_processor。学生正确解释 error 等级的过滤含义和 Logger 在 try 外的作用域，老师补充 completed 表示处理及保存成功。日志理解检查通过，模块 5 各知识点的学习检查均已通过。学生已在创建处理器后、调用 processor->process(image) 前补上 stage="process_image"，老师已只读确认，不要求追加运行观察。未展示的退出码不记作已验证。老师未配置、构建、运行或测试。

## 2. 日志是什么，和结果有什么区别

处理结果回答“算出了什么”，例如最终线段坐标。日志记录“程序运行过程中发生了什么”，例如开始处理哪张图片、何时完成、在哪个阶段失败。

| 内容 | 本项目中的去向 | 用途 |
| --- | --- | --- |
| 图片和 result.json | 文件 | 保存计算结果 |
| Width、Selected line segments 等已有输出 | std::cout | 当前教学中的人工观察 |
| 开始、完成、错误日志 | std::cerr | 记录运行事件，辅助定位问题 |

std::cout 和 std::cerr 默认都可能显示在同一个终端，但它们是不同的输出流。cerr 用于诊断输出，不意味着写到它的内容只能是错误；本课 info 和 error 日志都写到这里。两条流可以分别重定向。标准流的对应关系可查阅 [C++ 标准草案](https://eel.is/c++draft/iostream.objects.overview)。

仅有 result.json 不足以回答“为什么这次没有成功产生结果”。日志可以保留失败原因，但日志本身不会修复错误，也不决定进程退出码；退出码仍由 main 的 return 决定。

## 3. 日志等级、时间与上下文

### 3.1 两种等级足够开始

本课先与原项目的基础等级对齐：

| 日志等级 | 何时使用 | 例子 |
| --- | --- | --- |
| info | 正常的重要事件 | 开始处理、处理完成 |
| error | 本次操作失败 | 无法读图、配置不合法、保存失败 |

log_level 是显示规则：

| 配置值 | 显示 info | 显示 error |
| --- | --- | --- |
| info（默认） | 是 | 是 |
| error | 否 | 是 |

将等级设为 error，不会让程序产生错误；成功运行时可能完全没有日志。等级只过滤通过 Logger 发出的日志，不影响已有 std::cout 输出和结果保存。更多工具还提供 debug、warning 等等级，但本课只实现表中的两种。

### 3.2 一条日志需要哪些信息

下面是一条示意日志，时间数字仅为示例：

```json
{"time_ms":1000,"level":"info","message":"Processing started","context":{"input":"samples/frame.jpg","processor":"lane_lines","max_lines":2}}
```

- time_ms：事件发生时的系统时钟时间戳，以毫秒表示，不是程序耗时。
- level：info 或 error。
- message：事件名称或简短说明。
- context：相关信息，例如输入路径、处理器名称、参数或出错阶段。

“Processing failed”只能说明失败；附上 stage=read_image 和具体路径，就能知道失败发生在哪一步。上下文按事件选取，不需要把所有像素或全部程序状态塞进日志。

### 3.3 为什么每条日志只占一行

本课用 record.dump() 输出紧凑 JSON，再追加一个换行符。每一行是一条独立 JSON 记录，方便按行查看或逐条解析。多行这样的记录不是一个整体 JSON 对象或数组。

结果文件仍用 dump(2) 方便阅读；日志用 dump() 维持一条事件一行。库会转义消息字符串中的换行等字符。参见 [dump 文档](https://json.nlohmann.me/api/basic_json/dump/)。

## 4. 老师已准备的素材

两份文件都在 `/home/rikanshin/VisionFlow/VisionFlow_learn/configs/`：

| 文件 | 内容 |
| --- | --- |
| logging-info.json | {"max_lines": 2, "log_level": "info"} |
| logging-error.json | {"max_lines": 2, "log_level": "error"} |

已有配置不需要改动：缺少 log_level 时使用 info。新增字段要同时加入加载器的合法字段列表，否则旧加载器会报 unknown config key。

## 5. 具体代码与修改位置

本课只修改 `/home/rikanshin/VisionFlow/VisionFlow_learn/apps/vision_cli.cpp`。先读懂概念，再按下面的明确位置一次完成这一知识点。

### 5.1 添加头文件和 Logger 类

在 include 区域的 `#include <nlohmann/json.hpp>` 下面添加：

```cpp
#include <chrono>
```

在所有 include 之后、现有 `struct Config` 之前插入下面完整的类：

```cpp
class Logger {
public:
    void set_level(const std::string& level) {
        if (level != "info" && level != "error") {
            throw std::invalid_argument("log_level must be info or error");
        }
        errors_only_ = (level == "error");
    }

    void info(const std::string& message,
              const nlohmann::json& context = nlohmann::json::object()) const {
        if (!errors_only_) {
            write("info", message, context);
        }
    }

    void error(const std::string& message,
               const nlohmann::json& context = nlohmann::json::object()) const {
        write("error", message, context);
    }

private:
    bool errors_only_ = false;

    void write(const std::string& level, const std::string& message,
               const nlohmann::json& context) const {
        const auto now = std::chrono::system_clock::now();
        const auto time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        nlohmann::json record = nlohmann::json::object();
        record["time_ms"] = time_ms;
        record["level"] = level;
        record["message"] = message;
        record["context"] = context;

        std::cerr << record.dump() << '\n';
    }
};
```

Logger 负责统一格式与等级过滤，调用处只描述发生了什么。info 会检查 errors_only_，error 总是记录。默认参数允许不带上下文地写 `logger.info("Processing started")`；下面的项目代码会显式补充上下文。

时间相关的三步是：now() 取得当前系统时间点，time_since_epoch() 取得从该时钟纪元起经过的时长，duration_cast<milliseconds> 将单位转换为毫秒，count() 取数值。当前 Linux 环境的系统时钟使用 Unix 时间基准。它适合标识事件时间；系统时钟可以被校准，测量耗时通常应使用 steady_clock，本课不计算耗时。参考 [system_clock 文档](https://eel.is/c++draft/time.clock.system)。

这是当前单线程 CLI 使用的教学实现。原项目另有线程锁、线程编号和请求编号，后续学习并发与协议时再对齐。

### 5.2 给 Config 和加载器加入日志等级

将现有 Config 结构体替换为：

```cpp
struct Config {
    int max_lines = 3;
    std::string log_level = "info";
};
```

在 load_config 的字段遍历中找到：

```cpp
        if (it.key() != "max_lines") {
```

替换为：

```cpp
        if (it.key() != "max_lines" && it.key() != "log_level") {
```

原有抛错语句继续保留。这个 && 表示：既不是 max_lines，也不是 log_level，才是未知字段。

在 load_config 的 max_lines 读取与校验结束后、`return config;` 之前插入：

```cpp
    const auto level_it = document.find("log_level");
    if (level_it != document.end()) {
        if (!level_it.value().is_string()) {
            throw std::invalid_argument("log_level must be a string");
        }
        const std::string level = level_it.value().get<std::string>();
        if (level != "info" && level != "error") {
            throw std::invalid_argument("log_level must be info or error");
        }
        config.log_level = level;
    }
```

这里再次使用上一课的原则：缺省则保留默认值；提供字段则检查类型和允许的值。set_level 自身也校验，以便直接调用 Logger 时得到明确错误。

### 5.3 在 try 外创建 Logger 和阶段变量

找到 main 开头：

```cpp
int main(int argc, char* argv[]) {
    try {
```

替换为：

```cpp
int main(int argc, char* argv[]) {
    Logger logger;
    std::string stage = "arguments";

    try {
```

Logger 和 stage 定义在 try 外，后面的两个 catch 也能访问它们。如果定义在 try 内，离开该作用域后 catch 无法使用这些局部变量。

stage 保存当前阶段。错误信息通常说明原因；阶段说明程序当时正在做什么。

### 5.4 配置加载后设置等级并记录开始

在 main 中找到配置加载块：

```cpp
        Config config;
        if (!config_path.empty()) {
            config = load_config(config_path);
        }
```

将它替换为：

```cpp
        stage = "config";
        Config config;
        if (!config_path.empty()) {
            config = load_config(config_path);
        }
        logger.set_level(config.log_level);

        logger.info("Processing started", {
            {"input", input_path},
            {"processor", processor_name},
            {"max_lines", config.max_lines}
        });
```

这里的 `{{"input", input_path}, ...}` 是 nlohmann/json 支持的对象初始化写法，每一对花括号是一组键和值。

如果配置本身读取失败，set_level 尚未执行，但 try 外创建的 Logger 已可用，默认等级允许输出错误日志。

欢迎语和后面的统计输出暂时保留。--help 在命令行循环内就返回，因此正常查看帮助不会产生日志或读配置。

### 5.5 在读图、处理和保存之前更新阶段

以下分别是在现有语句之前插入一行，不要删除原语句：

在 `cv::Mat image = cv::imread(input_path, cv::IMREAD_COLOR);` 之前插入：

```cpp
        stage = "read_image";
```

将创建处理器和调用 process 的相邻位置整理为下面四行，以分别标明两个阶段：

```cpp
        stage = "create_processor";
        auto processor = create_processor(processor_name, config.max_lines);
        stage = "process_image";
        const ProcessResult result = processor->process(image);
```

你已在创建处理器之前设置 create_processor；本次只需补上调用 process 之前的那一行 stage 更新。否则真正的图像处理失败也会沿用创建阶段的名称。

在第一处 `if (!cv::imwrite("build/edges.png", result.edges))` 之前插入：

```cpp
        stage = "save_outputs";
```

后面的图片和 JSON 保存都属于 save_outputs。本课无需给每一行代码单独设置阶段。

### 5.6 让读写失败统一进入 catch

现有的若干失败分支直接输出 std::cerr 然后 return 1。为了统一日志格式，将这些分支内部的两条语句替换为一条 throw；外层 if 保留。

读图失败的完整 if 改为：

```cpp
        if (image.empty()) {
            throw std::runtime_error("cannot read image: " + input_path);
        }
```

四处 imwrite 失败分支分别改为下面的形式，位置仍在原处：

```cpp
        if (!cv::imwrite("build/edges.png", result.edges)) {
            throw std::runtime_error("cannot save image: build/edges.png");
        }
```

```cpp
            if (!cv::imwrite("build/roi_mask.png", result.roi_mask)) {
                throw std::runtime_error("cannot save image: build/roi_mask.png");
            }
```

```cpp
            if (!cv::imwrite("build/roi_edges.png", result.roi_edges)) {
                throw std::runtime_error("cannot save image: build/roi_edges.png");
            }
```

```cpp
        if (!cv::imwrite("build/preview.png", result.preview)) {
            throw std::runtime_error("cannot save image: build/preview.png");
        }
```

ROI 保存外面的非空判断、所有成功保存提示都保留。JSON 文件打开和写入失败已经使用 throw，不需要修改。

统一在 catch 记录错误，可以避免底层先记录一次、外层再重复记录一次。抛异常之后不会继续执行 try 中的后续处理。

### 5.7 全部结果保存成功后记录完成

找到 main 末尾、JSON 文件成功保存后的两行：

```cpp
        std::cout << "Saved: " << output_path << "\n";

        return 0;
```

在保存提示之后、return 0 之前插入：

```cpp
        logger.info("Processing completed", {
            {"processor", processor_name},
            {"line_count", result.lines.size()},
            {"result_path", output_path}
        });
```

只有所有保存操作成功后才记录 completed，避免在结果尚未写完时误报完成。

### 5.8 替换 main 末尾两个 catch

将现有的两个 catch 连同其内容替换为：

```cpp
    } catch (const cv::Exception& error) {
        logger.error("OpenCV operation failed", {
            {"stage", stage},
            {"error", error.what()}
        });
        return 1;
    } catch (const std::exception& error) {
        logger.error("Processing failed", {
            {"stage", stage},
            {"error", error.what()}
        });
        return 1;
    }
```

main 的最后一个右花括号保留。异常捕获顺序仍保持 OpenCV 专用类型在前、通用类型在后。两种失败都只记录一条本程序的 error 日志并返回 1。

按上面的代码完成后，本程序直接向 std::cerr 输出的地方集中在 Logger::write。OpenCV 自己也可能向标准错误流输出诊断文字，因此不能假设收集到的所有 stderr 内容都来自我们的 JSON Logger。

## 6. 运行观察参考

以下命令由学生选择执行，老师未执行；不补做之前已经跳过的配置错误实验。

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake --build build
./build/vision_cli --processor lane_lines --config configs/logging-info.json
```

预期：正常结果仍保留两条线段；另外看到 Processing started 和 Processing completed 两条 info 日志。时间数字随运行变化，JSON 字段顺序不必与示例一致。

切换为只记录错误：

```bash
./build/vision_cli --processor lane_lines --config configs/logging-error.json
```

预期：成功运行没有 Logger 日志，但原有 Width、Saved 等 std::cout 输出仍出现，结果文件照常生成。这是日志过滤成功，不是程序没有运行。

如果想观察 error 不被过滤，可以在本课主动选择不支持的处理器名称：

```bash
./build/vision_cli --processor unknown --config configs/logging-error.json
echo $?
```

样例图片可读取时，预期日志的 level 为 error，context.stage 为 create_processor，context.error 包含 unknown processor: unknown，退出码为 1。没有要求现在必须执行这个错误实验。

### 把日志与普通输出分别保存

下面也是可选观察方式：

```bash
./build/vision_cli --processor lane_lines --config configs/logging-info.json > build/run-output.txt 2> build/run.log
```

`>` 保存标准输出，`2>` 保存标准错误流；终端不再直接显示这两条流的内容。它们会覆盖对应文件，`2>>` 则是追加标准错误流内容。Logger 仍只负责写 cerr，实际写到 run.log 是终端重定向的作用。

这不改变程序内部写出的 result.json。result.json 保存计算结果，run.log 保存运行事件。

## 7. 本知识点的理解检查

完成后告诉老师，由老师只读检查实现；可提供实际运行结果，再说明两点：

1. 设置 log_level=error 后，程序成功运行没有日志，为什么是正常现象？
2. 为什么 Logger 定义在 try 外，且 completed 日志放在所有结果保存成功之后？

日志是模块 5 的最后一个知识点。本课实现和理解确认后再总结模块 5；模块 6 的 VFLW 字节协议尚未开始，届时再询问熟悉程度。
