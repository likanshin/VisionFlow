# 第四模块·第三步 B：声明、头文件与编译链接

## 当前安排

学生最初表示部分了解，经讲解后确认理解，并自行完成头文件、实现文件、主程序包含关系及 CMake 的调整。老师已只读核对文件内容：声明与定义匹配，构建配置已列出实现文件并配置 include 搜索路径。学生现已提供拆分后的成功构建与运行输出，仍得到 2095 个全图边缘、1088 个 ROI 边缘以及最终 3 条线段，与此前一致。下文保留为原理复习。

## 1. 声明告诉调用者怎样使用函数

以现有函数为例：

```cpp
double line_length(const cv::Vec4i& segment);
```

这是函数声明：名字是 line_length，接收一条线段的只读引用，返回 double。末尾是分号，没有函数体。编译器见到它，就能检查后续调用的参数与返回值使用是否符合声明。

下面是定义：

```cpp
double line_length(const cv::Vec4i& segment) {
    const double dx = segment[2] - segment[0];
    const double dy = segment[3] - segment[1];
    return std::hypot(dx, dy);
}
```

定义包含函数体，说明实际执行哪些操作。函数定义也是一种声明，所以之前将定义放在 main 前面即可直接调用。

声明和定义需要对应一致的函数类型和作用域；否则可能声明了一个函数、定义了另一个函数。本课例子中，const 和引用符号都需要匹配。

## 2. 为什么通常拆成 hpp 和 cpp

我们接下来准备采用这样的结构：

```text
VisionFlow_learn/
├── include/line_utils.hpp     共享声明
├── src/line_utils.cpp         两个函数的实现
└── apps/vision_cli.cpp        main，调用这些函数
```

这样 main 和未来其他使用者都可以包含同一个头文件，获得一致的声明，而不用各自手写一份。

示意头文件内容：

```cpp
#pragma once

#include <cstddef>
#include <vector>
#include <opencv2/core.hpp>

double line_length(const cv::Vec4i& segment);
void rank_and_limit(std::vector<cv::Vec4i>& lines,
                    std::size_t max_lines);
```

头文件要包含声明自身需要的类型依赖，这样使用者可以直接包含它。`#pragma once` 防止这个头文件在同一次编译单元的处理中被重复展开；它不表示整个项目只处理一次这个头文件。

普通非 inline 函数的定义通常放在 cpp 文件中。若把这样的定义放在头文件，再让多个 cpp 包含它，就可能产生多个定义并在链接时报重复定义。模板和 inline 有另外的规则，以后讲解。

## 3. include 做了什么

调用方写：

```cpp
#include "line_utils.hpp"
```

在预处理阶段，可以把它理解为将指定头文件的内容展开到这里。编译器随后就能看到函数声明。

它不会根据头文件名字自动寻找并编译同名 cpp，也不会在程序运行时读取头文件。

实现文件也应包含自己的头文件，让编译器检查实现与公开声明的一致性，并取得相关类型声明。

头文件后缀 hpp 是 C++ 项目常用约定，不具有“自动链接实现”的特殊能力。通常也不通过 include cpp 来连接不同实现文件，而是把各 cpp 分别加入构建。

## 4. 两个 cpp 怎样成为一个程序

```text
vision_cli.cpp + 展开的头文件 → 编译 → vision_cli.cpp.o ─┐
                                                      ├→ 链接 → vision_cli
line_utils.cpp + 展开的头文件 → 编译 → line_utils.cpp.o ─┘
OpenCV 等所需库 ───────────────────────────────────────┘
```

每个 cpp 连同其展开的头文件，形成一个编译单元。编译器通常分别处理每个编译单元，产生目标文件（Linux 中常以 .o 结尾）。

编译 main 时，函数声明让编译器知道如何生成调用代码；对另一个 cpp 中函数的引用可留到链接阶段解决。

链接器把目标文件和所需库组合起来，将函数引用与对应实现连接起来。

因此常见问题发生在不同阶段：

| 情况 | 常见错误阶段与提示 |
| --- | --- |
| 找不到所包含的头文件 | 预处理阶段提示 No such file or directory |
| 调用处没见到声明 | 编译阶段提示 was not declared in this scope |
| 有声明，但实现文件未参与构建、链接 | 链接阶段提示 undefined reference |
| 同一个普通函数在多个目标文件中被定义 | 链接阶段通常提示 multiple definition |

具体错误文字取决于编译器、链接器及语言设置；这里用本项目常见的 GCC 工具链提示举例。

## 5. CMake 分别负责哪几件事

拆文件时，构建目标需要包含两个源文件：

```cmake
add_executable(vision_cli
    apps/vision_cli.cpp
    src/line_utils.cpp
)
```

这告诉构建系统编译两个 cpp，并把结果用于同一个可执行程序。仅将实现文件保存到 src 目录，并不会自动把它加入当前显式列出的目标。

头文件放在 include 目录时，需要提供搜索路径：

```cmake
target_include_directories(vision_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
)
```

这让 `#include "line_utils.hpp"` 能找到我们自己的头文件，也保留 OpenCV 头文件搜索路径。

现有的库链接继续保留：

```cmake
target_link_libraries(vision_cli PRIVATE ${OpenCV_LIBS})
```

这三项各有职责：列出要编译的源文件、指定头文件搜索路径、指定需要链接的库。头文件搜索路径本身不会把 cpp 加入编译，也不会替代链接库设置。

按约定 CMake 辅助配置可由老师维护；本次学生已主动完成配置和文件拆分，上述示例与当前结构对应。

## 6. 理解检查

假设你已经：

1. 在 line_utils.hpp 中声明 line_length；
2. 在 line_utils.cpp 中定义 line_length；
3. 在 vision_cli.cpp 中包含头文件并调用它；
4. 正确配置头文件搜索路径，但 add_executable 仍然只列出 vision_cli.cpp，其他地方也没有提供该函数实现。

为什么头文件能找到、调用处编译也能通过，但最终生成可执行程序仍会失败？问题主要出在编译阶段还是链接阶段？

学生已确认理解并完成拆分。下一步在学习目录自行验证：

```bash
cmake --build build
./build/vision_cli
echo $?
```

预期仍保留原来的 3 条线段，长度为 257.002、255.601、254.201，正常退出码为 0；如有错误，先反馈完整输出。老师不代为运行。
