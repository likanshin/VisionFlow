# 第五模块：配置读取与校验

## 1. 本课目标与进度

学生已完成命令行和 JSON 输出课程，对配置读取与校验要求从头讲解。本知识点的概念、具体代码、运行参考和理解检查集中在这一份讲义。

目标：将道路线段的最大保留数量从代码里的固定 3 改为配置中的 max_lines；使用 --config 选择文件，在处理图片之前检查配置是否合法。

当前状态：学生已完成配置代码并提供成功构建及正常配置运行结果：lane_lines 使用 configs/vision.json 时保留 2 条线段，result.json 的 max_lines 为 2，lines 为 [82,358,287,203]、[352,202,556,356]。老师已只读核对配置读取、默认值、未知字段拒绝、类型和范围检查，以及 main→工厂→道路处理器→处理函数→rank_and_limit 的参数传递。学生明确跳过错误配置的运行观察，不再催补；默认配置也没有本课运行证据。学生起初不理解缺省与错误输入的区别，老师已解释“缺省使用默认值、提供了错误值则报错”，随后学生正确判断 300 是范围错误，配置基础理解检查通过，本知识点已完成。未运行的分支不记作已验证。下一知识点为日志，先询问熟悉程度。老师未配置、构建、运行或测试。本课无需修改 CMake。

## 2. 配置文件解决什么问题

现在 src/processor.cpp 中写着：

```cpp
    const std::size_t max_lines = 3;
    rank_and_limit(result.lines, max_lines);
```

想改为最多保留 2 条线段，就要修改源码并重新编译。将这个数放到配置文件后，程序每次启动读取它；之后只改配置即可改变数量，不必为了调整这个值重新编译。

配置也是 JSON，但用途和 result.json 不同：

| 文件 | 谁提供 | 程序怎样使用 |
| --- | --- | --- |
| configs/vision.json | 使用者填写；本课由老师准备初始素材 | 程序读取，决定怎样处理 |
| build/result.json | 程序生成 | 保存已经得到的处理结果 |

本课的配置文件只有一个参数：

```json
{
  "max_lines": 2
}
```

max_lines 控制排序后最多保留多少条线段；它不会把未检测到的线段创造出来。若只找到 1 条合格线段，即使上限为 2，结果也只有 1 条。

本课沿用原项目 max_lines 的 1～256 范围限制，但默认值仍保留学习工程的 3。256 是本项目的参数约定，不是 JSON 或 OpenCV 强制要求的上限。其他算法参数保持现状。

## 3. 从文件到算法的完整路径

```text
--config configs/vision.json
        ↓ 取得文件路径
打开文件 → 解析 JSON → 检查对象、字段、类型与范围
        ↓ 返回配置结构体
Config.max_lines
        ↓ main 传给工厂
create_processor(processor_name, config.max_lines)
        ↓ 构造道路处理器，保存数量上限
LaneLinesProcessor.max_lines_
        ↓ process 传给处理函数
process_lane_lines(image, max_lines_)
        ↓ 排序后截取
rank_and_limit(result.lines, max_lines)
```

配置不只要“读出来”，还要真正传到使用它的位置。任何一层仍固定写 3，都可能导致修改配置没有效果。

我们使用 `Config` 结构体表示通过校验后的配置；JSON 是外部表示，Config 是程序内部使用的类型。处理器只接收一个数量，不需要知道文件路径或 JSON 语法。

## 4. 默认值、错误值与校验顺序

规则如下：

| 情况 | 本课行为 |
| --- | --- |
| 不传 --config | 使用默认 max_lines=3，不打开任何配置文件 |
| 配置为 {} | 字段没提供，保留默认值 3 |
| 配置为 {"max_lines": 2} | 使用 2 |
| 显式指定的文件不存在 | 报错，不静默退回默认值 |
| JSON 语法错误 | 解析时报错 |
| 顶层是 [] 或一个数字 | 报错，配置必须是对象 |
| 未知字段，如 max_line | 报错，避免拼错却没生效 |
| max_lines 是 "2"、2.5、2.0、true 或 null | 报错，本课要求整数表示 |
| max_lines 为 0、负数或大于 256 | 报错，范围必须为 1～256 |

这里特意区分“没有写”和“写了但不合法”。默认值用于补充缺省字段；不应掩盖明显的输入错误。

检查顺序是：文件能否打开 → JSON 能否解析 → 顶层结构 → 字段名称 → 值类型 → 值范围 → 转成内部类型。

不能先用 get<int>() 转换再检查：浮点数转换时可能失去小数部分，大整数也不能靠窄类型转换来验证范围。本课先确认整数类型，再检查 1～256 的小范围，最后取出 int。

## 5. 老师已准备的配置素材

以下文件位于 `/home/rikanshin/VisionFlow/VisionFlow_learn/configs/`：

| 文件 | 内容 | 用途 |
| --- | --- | --- |
| vision.json | {"max_lines": 2} | 观察最终线段从 3 条变为最多 2 条 |
| defaults.json | {} | 观察缺省字段使用 3 |
| invalid-max-lines.json | {"max_lines": 0} | 观察范围错误 |
| invalid-max-lines-type.json | {"max_lines": "2"} | 观察类型错误 |

素材已创建，你不必重复新建。错误文件是供学习时主动选择的输入，不是程序启动时自动读取的配置。

## 6. 具体代码与修改位置

本课由你修改以下三个文件。先完成这些修改再构建；不必每改完一处就停下来做理解检查。

| 文件 | 修改内容 |
| --- | --- |
| /home/rikanshin/VisionFlow/VisionFlow_learn/apps/vision_cli.cpp | Config、加载函数、--config、传参及结果记录 |
| /home/rikanshin/VisionFlow/VisionFlow_learn/include/processor.hpp | 道路处理器的构造参数和成员、函数声明 |
| /home/rikanshin/VisionFlow/VisionFlow_learn/src/processor.cpp | 保存并传递上限，替换固定的 3 |

### 6.1 在 CLI 的 main 之前添加配置类型和加载函数

打开 apps/vision_cli.cpp。现有 `<fstream>`、`<string>`、`<stdexcept>` 和 `<nlohmann/json.hpp>` 已足够，不需要重复添加。

在所有 include 之后、`int main(int argc, char* argv[])` 之前，插入下面完整代码：

```cpp
struct Config {
    int max_lines = 3;
};

Config load_config(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open config: " + path);
    }

    const nlohmann::json document = nlohmann::json::parse(file);
    if (!document.is_object()) {
        throw std::invalid_argument("config must be an object");
    }

    for (auto it = document.begin(); it != document.end(); ++it) {
        if (it.key() != "max_lines") {
            throw std::invalid_argument("unknown config key: " + it.key());
        }
    }

    Config config;
    const auto it = document.find("max_lines");
    if (it != document.end()) {
        const auto& value = it.value();
        if (!value.is_number_integer()) {
            throw std::invalid_argument("max_lines must be an integer");
        }

        const double number = value.get<double>();
        if (number < 1 || number > 256) {
            throw std::invalid_argument("max_lines must be 1..256");
        }
        config.max_lines = value.get<int>();
    }

    return config;
}
```

逐段解释：

- std::ifstream 是文件输入流，对应上一课的输出流 std::ofstream。
- parse(file) 将文件里的 JSON 文本解析为内存对象；非法语法会抛异常，本课沿用 main 的通用 catch 报错。它还检查输入末尾，不接受一个 JSON 文档后再混入其他文本。参见 [parse 文档](https://json.nlohmann.me/api/basic_json/parse/)。
- is_object() 要求根节点为对象。合法 JSON 可以是数组，但合法 JSON 不等于合法配置。
- 遍历对象时，it.key() 是字段名称。这里仅接受 max_lines，拼错字段直接报错。
- Config config 创建带默认值 3 的内部配置；find 找不到字段时返回 end，因此跳过赋值，默认值自然保留。参见 [find 文档](https://json.nlohmann.me/api/basic_json/find/)。
- it.value() 取得字段值；is_number_integer() 接受库中的有符号和无符号整数，不接受字符串、布尔值或浮点数表示。参见 [整数判断文档](https://json.nlohmann.me/api/basic_json/is_number_integer/)。
- 此处先转为 double 检查 1～256，再转 int。这个小区间内的整数都能被 double 精确表示，较大的数也不会因直接转成 int 而先发生窄化。它是本课小范围参数的做法，不应原样套到要求精确校验整个 64 位整数范围的场景。get<T>() 用于提取或转换值，不代替类型和业务校验。参见 [get 文档](https://json.nlohmann.me/api/basic_json/get/)。

默认值只在字段缺省时起效；`{"max_lines": null}` 能找到字段，所以会进入检查并被拒绝。

配置读取暂放在 CLI 上方，便于本课集中阅读；以后需要复用时可提取头文件与源文件，本课不重做已掌握的文件拆分练习。

### 6.2 修改 processor.hpp，允许道路处理器保存上限

在 include/processor.hpp 中找到完整的 LaneLinesProcessor 类，将它替换为：

```cpp
class LaneLinesProcessor : public IProcessor {
public:
    explicit LaneLinesProcessor(int max_lines = 3);
    ProcessResult process(const cv::Mat& image) override;

private:
    int max_lines_;
};
```

这是为对象增加一个构造参数和一个私有成员。max_lines 是传入值，max_lines_ 是对象保存的值；尾部下划线只是命名习惯。explicit 避免把一个整数意外地隐式转换成处理器对象。

文件末尾现有两个普通函数声明替换为：

```cpp
// 输入为非空的 8 位 BGR 图像；max_lines 必须为 1..256。
ProcessResult process_lane_lines(const cv::Mat& image, int max_lines = 3);
std::unique_ptr<IProcessor> create_processor(const std::string& name,
                                             int max_lines = 3);
```

默认参数让原来的单参数调用仍可使用默认 3。默认值只写在头文件声明中，源文件定义不要重复写 `= 3`。

IProcessor 和 EdgesProcessor 的接口保持现状：数量上限仅影响道路模式。

### 6.3 修改 processor.cpp，完成数量的传递

在 src/processor.cpp 的 include 区域之后、现有 LaneLinesProcessor::process 之前，添加：

```cpp
LaneLinesProcessor::LaneLinesProcessor(int max_lines)
    : max_lines_(max_lines) {
    if (max_lines_ < 1 || max_lines_ > 256) {
        throw std::invalid_argument("max_lines must be 1..256");
    }
}
```

冒号后是成员初始化列表：对象构造时将传入的 max_lines 保存到 max_lines_。构造函数没有返回类型。这里也检查范围，使其他代码直接创建道路处理器时仍能得到明确错误。

将现有道路处理器的 process 定义替换为：

```cpp
ProcessResult LaneLinesProcessor::process(const cv::Mat& image) {
    return process_lane_lines(image, max_lines_);
}
```

再找到普通处理函数的这一行：

```cpp
ProcessResult process_lane_lines(const cv::Mat& image) {
```

替换为下面的函数头与开头检查，原有 `ProcessResult result;` 及后续算法接在它后面：

```cpp
ProcessResult process_lane_lines(const cv::Mat& image, int max_lines) {
    if (max_lines < 1 || max_lines > 256) {
        throw std::invalid_argument("max_lines must be 1..256");
    }
```

这个普通函数也可被直接调用，因此同样保护它的输入。不要提前添加函数结尾的右花括号，原来的完整函数体继续保留。

在该函数内部找到：

```cpp
    const std::size_t max_lines = 3;
    rank_and_limit(result.lines, max_lines);
```

将这两行一起替换为：

```cpp
    rank_and_limit(result.lines, static_cast<std::size_t>(max_lines));
```

旧的局部变量必须删除，否则会与函数参数重名。这里已经保证 max_lines 为正数，再转换为 rank_and_limit 所需的无符号数量类型。

最后，将文件末尾的完整 create_processor 定义替换为：

```cpp
std::unique_ptr<IProcessor> create_processor(const std::string& name,
                                             int max_lines) {
    if (max_lines < 1 || max_lines > 256) {
        throw std::invalid_argument("max_lines must be 1..256");
    }
    if (name == "edges") {
        return std::make_unique<EdgesProcessor>();
    }
    if (name == "lane_lines") {
        return std::make_unique<LaneLinesProcessor>(max_lines);
    }
    throw std::invalid_argument("unknown processor: " + name);
}
```

make_unique 后面的参数会转交给构造函数。edges 不使用数量上限，但配置必须合法；工厂的范围检查也保护不经文件加载而直接调用它的代码。各入口的范围规则都保持 1～256。

### 6.4 在 main 中接入 --config

回到 apps/vision_cli.cpp，在 try 开头的 `std::string processor_name = "edges";` 之后添加：

```cpp
        std::string config_path;
```

空字符串表示没有指定配置文件。接着将整个现有 for 参数循环替换为下面的版本；此代码已经包含原有选项，不要再追加第二个循环：

```cpp
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];

            if (arg == "--help") {
                std::cout
                    << "Usage: vision_cli [--input PATH] [--processor NAME] [--config PATH] [--help]\n"
                    << "  --input PATH      Input image (default: samples/frame.jpg)\n"
                    << "  --processor NAME  edges or lane_lines (default: edges)\n"
                    << "  --config PATH     JSON configuration (default: max_lines=3)\n"
                    << "  --help            Show this help and exit\n";
                return 0;
            }

            if (arg == "--input" || arg == "--processor" || arg == "--config") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("missing value for " + arg);
                }

                const std::string value = argv[i + 1];
                if (value.empty() ||
                    (value.size() >= 2 && value[0] == '-' && value[1] == '-')) {
                    throw std::invalid_argument("missing value for " + arg);
                }

                ++i;
                if (arg == "--input") {
                    input_path = value;
                } else if (arg == "--processor") {
                    processor_name = value;
                } else {
                    config_path = value;
                }
            } else {
                throw std::invalid_argument("unknown argument: " + arg);
            }
        }
```

在这个循环结束之后、欢迎输出之前插入：

```cpp
        Config config;
        if (!config_path.empty()) {
            config = load_config(config_path);
        }
```

先收集完选项再读取配置，让 --config 的位置不影响加载时机；帮助分支仍会在读文件之前退出。本课配置只含 max_lines，处理器名称仍由 --processor 选择，不涉及同名参数的覆盖优先级。

找到现有工厂调用：

```cpp
        auto processor = create_processor(processor_name);
```

替换为：

```cpp
        auto processor = create_processor(processor_name, config.max_lines);
```

### 6.5 在结果 JSON 中记录实际配置

在 main 后半段找到：

```cpp
        document["processor"] = processor_name;
```

在它下面添加：

```cpp
        document["max_lines"] = config.max_lines;
```

这样结果能同时说明“使用了什么上限”和“实际得到了几条线段”。edges 也会记录上限，但它不做线段检测，lines 仍为空数组。

原有图片保存、JSON 写文件和 catch 均保留。配置加载在读图之前；配置错误会直接报错并返回 1，不处理图片。

## 7. 自行构建与运行观察

以下命令由你执行，老师未执行。本课没有修改 CMake，因此完成所有源码修改后可直接构建：

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake --build build
./build/vision_cli --input samples/frame.jpg --processor lane_lines --config configs/vision.json
cat build/result.json
```

当前样例中，预期选中线段数变为 2，JSON 的 max_lines 为 2、lines 包含两条线段。这是本课预期，尚无本次运行证据。

然后可比较缺省和错误配置：

```bash
./build/vision_cli --processor lane_lines --config configs/defaults.json
./build/vision_cli --processor lane_lines --config configs/invalid-max-lines.json
echo $?
./build/vision_cli --processor lane_lines --config configs/invalid-max-lines-type.json
echo $?
```

defaults.json 保留默认上限 3；范围错误应提示 max_lines must be 1..256；类型错误应提示 max_lines must be an integer。后两种情况退出码应为 1。

无需补做以前明确跳过的无参数验证。本次错误运行不会删除旧 result.json，不应把旧文件当作本次成功输出。

修改配置值不需要重新编译；修改 C++ 代码仍需重新构建。若你将 vision.json 改为 1，可以直接重新运行观察。

## 8. 完成这一知识点后的理解检查

完成后告诉老师，由老师只读检查实现；可以提供实际运行结果，并解释：

1. 为什么 {} 使用默认 3，而 {"max_lines": "2"} 要报错？
2. JSON 解析成功为什么仍需要类型与范围校验？
3. 如果读取到 2，但最终仍保留 3 条线段，你会检查哪条参数传递路径？

先完成配置这一整份讲义，再进入日志知识点；不重复检查已掌握的命令行参数或 JSON 输出。
