# 第五模块：命令行参数

## 1. 范围与当前进度

本讲义完整覆盖 argc/argv、--input、--processor、默认值、参数错误处理和 --help。同一知识点的实现步骤集中在本文，不再拆成多份小课，也不要求每完成几行代码就停下来做一次理解检查。

当前状态：学生已完成 --input 解析并提供成功构建记录；指定 samples/frame.jpg 后输出 640×360、3 通道、2095 个边缘像素和 0 条最终线段，保存 edges.png 与 preview.png。未知选项和缺少值均按预期报错，缺少值后的退出码已确认为 1。老师只读核对代码，确认默认路径、参数消费、边界检查及异常捕获符合本步要求，处理器已恢复为 edges。学生正确解释读取的是 --input 后的路径值，参数下标理解检查通过，本小步已完成。学生明确跳过无参数调用的追加验证，后续不再催补；未展示的退出码不记作已验证。

最新进度：学生已实现完整命令行解析，老师只读确认 --input、--processor、空值与选项边界检查、--help 提前返回，以及 create_processor(processor_name) 的连接均符合讲义。学生提供了 --help 和显式 --processor edges 的运行输出：帮助只显示用法，edges 输出 2095 个边缘像素、0 条最终线段并保存图片。本次未展示退出码或 lane_lines 的命令行运行结果，不将它们记作已验证。2026-09-25，学生正确解释 --help 只需输出说明并直接退出、不继续执行后续程序，理解检查通过，命令行知识点已完成。不追加要求补做此前已跳过的验证。老师未构建、运行或测试。

老师负责讲义与只读检查，学生按讲义修改源码并自行构建、运行。老师没有执行课程实践。JSON、配置和日志各作为后续知识点讲解，不在本课提前实现。

## 2. 完成后支持的用法

```bash
./build/vision_cli --input samples/frame.jpg --processor lane_lines
./build/vision_cli --processor edges --input samples/frame.jpg
./build/vision_cli --help
```

| 选项 | 是否需要值 | 默认值或作用 |
| --- | --- | --- |
| --input | 需要图片路径 | samples/frame.jpg |
| --processor | 需要处理器名称 | edges；也支持 lane_lines |
| --help | 不需要 | 打印帮助并成功退出，不读取或处理图片 |

规则统一如下：

- 两个带值选项的顺序不限，重复指定时后一个值覆盖前一个。
- 缺少值、空字符串值、值的位置出现以 -- 开头的字符串，都报告当前选项缺少值。
- 未知选项报错；不支持的处理器名称交给已有工厂报错。
- 参数错误由现有 std::exception catch 输出，并返回 1；正常处理或帮助返回 0。
- 本课只支持 `--input 路径` 这种分开的形式，未提供 `--input=路径` 或短选项。
- 相对路径依然相对于启动程序时的工作目录，也可传绝对路径。含空格的路径要在终端加引号。
- 以 -- 开头的字符串保留给选项；真实文件名若以 -- 开头，可写成 `./--文件名.jpg`。

帮助采用简单的顺序解析规则：循环遇到 --help 就打印并返回，不再处理后面的参数。如果前面已经遇到参数错误，则先报错。因此 `--help --unknown` 显示帮助，`--unknown --help` 报错，`--input --help` 报 --input 缺少值。这是本课选择的行为，其他工具也可以采用不同规则。

## 3. argc/argv 与消费参数（已掌握，供查阅）

入口形式为：

```cpp
int main(int argc, char* argv[]) {
```

对于 `./build/vision_cli --input samples/frame.jpg --processor lane_lines`，argc 为 5：

| 下标 | argv 中的字符串 |
| --- | --- |
| 0 | ./build/vision_cli |
| 1 | --input |
| 2 | samples/frame.jpg |
| 3 | --processor |
| 4 | lane_lines |

循环从 1 开始跳过程序名称。读取下一项前要检查 `i + 1 < argc`；消费一个值后额外推进 i，循环末尾再前进一步到下一个选项。你已经正确解释了这个过程，不需要重复做理解检查。

把 argv[i] 放入 std::string 后再比较字符串内容；直接用 `argv[i] == "--input"` 比较的是指针。

## 4. 具体代码与修改位置

本课只修改 `/home/rikanshin/VisionFlow/VisionFlow_learn/apps/vision_cli.cpp`，无需改动 CMake、处理器类或工厂实现。

### 4.1 文件头部与 main 入口：已完成，保持现状

文件顶部已经有这些头文件：

```cpp
#include <string>
#include <stdexcept>
```

main 已经改成 `int main(int argc, char* argv[])`。这些内容不必重复添加。

### 4.2 替换 try 开头的默认值与参数循环

找到 try 内的：

```cpp
        std::string input_path = "samples/frame.jpg";
```

从这一行开始，直到现有 for 循环的结束右花括号，将整个区域替换成下面的代码。停止在“相对路径从运行程序时的工作目录开始计算”注释之前，保留读图及其后面的代码。

```cpp
        std::string input_path = "samples/frame.jpg";
        std::string processor_name = "edges";

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];

            if (arg == "--help") {
                std::cout
                    << "Usage: vision_cli [--input PATH] [--processor NAME] [--help]\n"
                    << "  --input PATH      Input image (default: samples/frame.jpg)\n"
                    << "  --processor NAME  edges or lane_lines (default: edges)\n"
                    << "  --help            Show this help and exit\n";
                return 0;
            }

            if (arg == "--input" || arg == "--processor") {
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
                } else {
                    processor_name = value;
                }
            } else {
                throw std::invalid_argument("unknown argument: " + arg);
            }
        }
```

这里提供的是同一知识点的完整解析代码，不要把它追加为第二个循环；若你已按旧版讲义加入 --processor，也用这个版本替换现有默认值和循环即可。

### 4.3 把欢迎输出移到解析之后

main 的开头、try 之前现在有这一行：

```cpp
    std::cout << "I am rebuilding VisionFlow.\n";
```

删去原位置的这一行，并在第 4.2 节的 for 循环结束后、读图注释之前插入：

```cpp
        std::cout << "I am rebuilding VisionFlow.\n";
```

这样 --help 只显示帮助；错误参数也不会先打印欢迎语。正常处理时仍会打印欢迎语。

### 4.4 读图继续使用 input_path：已完成

循环和欢迎语之后，已有的读图代码保持为：

```cpp
        // 相对路径从运行程序时的工作目录开始计算。
        cv::Mat image = cv::imread(input_path, cv::IMREAD_COLOR);
        if (image.empty()) {
            std::cerr << "Cannot read image: " << input_path << "\n";
            return 1;
        }
```

### 4.5 将解析出的名称传给工厂

在读图检查后，找到：

```cpp
        auto processor = create_processor("edges");
```

替换为：

```cpp
        auto processor = create_processor(processor_name);
```

紧接着的调用保持不变：

```cpp
        const ProcessResult result = processor->process(image);
```

原有统计、图片保存和两个 catch 都保留。文件末尾的通用 catch 已能处理参数错误，不要重复添加：

```cpp
    } catch (const std::exception& error) {
        std::cerr << "vision_cli: " << error.what() << "\n";
        return 1;
    }
```

## 5. 为什么这样组织

### 选项解析与处理器创建

--input 和 --processor 都需要一个值，因此共用边界检查和消费逻辑，再把值存到不同变量。也可以写成两个独立分支；合并能减少重复检查。

processor_name 是名称字符串，processor 是工厂创建的对象的智能指针。二者通过 `create_processor(processor_name)` 连接。若仍传固定的 "edges"，解析出的选择就不会生效。

合法名称判断继续由工厂负责。`--processor unknown` 有值，因此能通过语法解析，但会在工厂中报 `unknown processor: unknown`。当前 main 先读图再调用工厂，所以只有图片成功读取后才会到达这项名称检查。

### 缺少值与未知选项

`--processor` 位于最后时，`i + 1 >= argc` 为真，在访问下一项前抛异常。

`--input --processor edges` 虽然有下一项，但它以 -- 开头，因此仍报告 `missing value for --input`。value.empty() 处理 `--input ""`。

`value.size() >= 2` 先检查长度，再访问前两个字符；&& 从左往右求值，前面不成立时不会求值后面。这里用单引号字符 `'-'` 与一个字符比较。

arg 是 std::string 的独立副本，推进 i 后它仍保存当前选项名称；value 则保存当前选项的值。

### --help 为什么提前 return

--help 是不带值的开关，只需要输出说明。`return 0` 退出的是整个 main，因此后续不会读图、创建处理器或写出图片；仅使用 break 只能退出循环，程序还会继续处理图片。

帮助分支位于读图之前，所以即使指定了不存在的图片，只要解析成功到达 --help，也能显示帮助。例如 `--input missing.jpg --help` 不会尝试读取 missing.jpg。

输出帮助使用连续的 << 操作和普通字符串，不需要新库。

## 6. 自行构建与运行参考

以下命令由你选择执行，老师未执行。本次不要求补做已跳过的无参数验证，也不重复前一阶段已完成的检查。

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake --build build
./build/vision_cli --input samples/frame.jpg --processor edges
./build/vision_cli --input samples/frame.jpg --processor lane_lines
./build/vision_cli --help
echo $?
```

对当前样例和算法参数，edges 的最终线段数为 0；lane_lines 应显示 ROI 统计并保留 3 条线段。--help 应只显示帮助，退出码为 0。这些是预期行为，不能当成本次运行证据。

下面的边界情况供查阅或按需观察：

| 参数 | 预期行为 |
| --- | --- |
| --processor | missing value for --processor，返回 1 |
| --input --processor edges | missing value for --input，返回 1 |
| --input "" | missing value for --input，返回 1 |
| --unknown | unknown argument: --unknown，返回 1 |
| --processor unknown（图片可读取时） | unknown processor: unknown，返回 1 |
| --input missing.jpg --help | 显示帮助，不读图，返回 0 |

正常处理退出码为 0；若要观察某次调用的退出码，紧接着执行 echo $?。旧输出文件可能仍留在磁盘，不应仅凭文件存在判断本次是否生成。

## 7. 完成后的反馈

完成这一整份命令行讲义后告诉老师，由老师只读核对代码。可以附上你实际运行的结果，再用一句话说明为什么 --help 用 return 0 而不是 break。

已掌握的 argc/argv 下标和所有权不重复考察。之后进入 JSON 这一独立知识点时，再询问你的熟悉程度，并为它建立一份完整讲义。
