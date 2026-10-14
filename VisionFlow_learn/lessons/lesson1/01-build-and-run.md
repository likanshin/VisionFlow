# 第一课：让工程运行起来

## 本课目标

运行我们自己的 `vision_cli`，并能解释：源代码怎样变成程序，CMake 起什么作用，修改代码后为什么要重新构建。

原项目的 `apps/vision_cli.cpp` 最终负责读取 JPEG、调用图像算法和输出结果。我们先搭建它的最小入口。

## 1. 看懂目录

```text
VisionFlow_learn/
├── CMakeLists.txt                   构建说明
├── apps/
│   └── vision_cli.cpp               程序源代码
├── lessons/
│   └── lesson1/
│       └── 01-build-and-run.md      本课讲义
└── README.md                        学习路线与进度
```

第一次构建后会出现 `build/`，里面存放生成文件和可执行程序。它可以从源码重新生成，因此在本目录的 `.gitignore` 中忽略。

## 2. 看懂程序入口

打开 `apps/vision_cli.cpp`，核心只有以下几行：

```cpp
#include <iostream>

int main() {
    std::cout << "VisionFlow Learn: ready!\n";
    return 0;
}
```

- `#include <iostream>`：包含标准库中输入输出功能的声明，让编译器认识这里使用的 `std::cout`。
- `int main()`：程序的入口函数。`int` 表示它返回一个整数；花括号包围函数执行的语句。
- `std::cout`：标准输出流，通常显示在终端。`std::` 表示这个名字属于标准库的命名空间。
- `<<`：这里表示把右边的内容写到输出流中。
- `"..."`：字符串字面量；`\n` 表示换行。语句末尾的分号用于结束语句。
- `return 0;`：结束函数并返回成功状态。它不是打印数字 0；调用程序的 shell 可以读取这个退出码。

以 `//` 开头的是注释，帮助人阅读，不作为语句执行。

## 3. 看懂构建说明

`CMakeLists.txt` 中的几条设置分别表示：

| 内容 | 含义 |
| --- | --- |
| `cmake_minimum_required(VERSION 3.16)` | 要求 CMake 至少为 3.16 |
| `project(VisionFlowLearn LANGUAGES CXX)` | 项目名为 VisionFlowLearn，使用 C++ |
| `set(CMAKE_CXX_STANDARD 17)` | 使用 C++17 标准 |
| `set(CMAKE_CXX_STANDARD_REQUIRED ON)` | 必须满足指定标准，不退回旧标准 |
| `set(CMAKE_CXX_EXTENSIONS OFF)` | 不启用编译器专有语言扩展模式 |
| `add_executable(vision_cli apps/vision_cli.cpp)` | 从这个源文件构建名为 vision_cli 的可执行目标 |

`VisionFlowLearn` 是项目名，`vision_cli` 是可执行目标名，两者可以不同。`apps/vision_cli.cpp` 是相对于当前 `CMakeLists.txt` 的路径。

可以把构建过程理解为：

```text
CMakeLists.txt → CMake 配置并生成构建规则
                                  ↓
vision_cli.cpp → 编译器 → 目标文件 → 链接器 → vision_cli
```

CMake 负责组织构建，编译器负责翻译 C++，链接器把编译结果与需要的库组合成程序。`cmake --build` 会调用实际的构建工具来完成后面的步骤。

## 4. 亲自运行一次

在 WSL / Linux 终端逐行执行。本课使用系统里已有的 C++ 编译器、CMake 和 Ninja，不需要 Python 环境。Ninja 是负责执行构建规则的工具；当前机器已安装 Ninja，因此显式选用它。

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake -S . -B build -G Ninja
cmake --build build
./build/vision_cli
```

逐条理解：

1. `cd`：进入学习工程。
2. `-S .`：源码目录是当前目录；`-B build`：把构建文件写入 `build/`；`-G Ninja`：为 Ninja 生成构建规则。这一步配置工程并生成构建规则。
3. `cmake --build build`：按照生成的规则编译、链接程序。
4. `./build/vision_cli`：运行编译出来的程序。

预期输出：

```text
VisionFlow Learn: ready!
```

紧接着运行 `echo $?`，应看到 `0`。`$?` 表示上一条命令的退出码，因此不要在这两条命令之间执行其他命令。

## 5. 你的动手练习

把程序的输出文字改成：

```text
I am rebuilding VisionFlow.
```

先预测：只保存源文件，直接运行 `./build/vision_cli`，会显示旧文字还是新文字？再实际运行验证。

然后执行 `cmake --build build` 并再次运行程序，观察输出的变化。暂时不要改目标名称或加入图像处理代码。

## 6. 理解检查

请把修改后的运行输出，以及下面三个问题的回答发给老师：

1. `apps/vision_cli.cpp` 和 `build/vision_cli` 分别是什么？修改前者后，为什么后者不会立刻改变？
2. `cmake -S . -B build -G Ninja` 和 `cmake --build build` 分别完成什么工作？
3. `std::cout` 输出的文字与 `return 0` 返回的值，各自有什么用途？

可以用自己的话回答，不需要背术语。如果卡住，请贴出执行的命令和完整报错；我们留在这一课一起解决。

完成练习、解释清楚并确认理解后，才进入模块 2。
