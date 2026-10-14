# 第四课问答记录：霍夫参数、头文件与接口设计

> 配套讲义：`04-1-road-roi.md`、`04-2-hough-lines.md`、`04-2b-filter-lines.md`、`04-2c-rank-and-limit.md`、`04-3a-extract-functions.md`、`04-3b-headers-and-linking.md`、`04-3c-process-result.md`
> 代码：`apps/vision_cli.cpp`、`include/line_utils.hpp`、`src/line_utils.cpp`、`include/processor.hpp`、`src/processor.cpp`
> 处理流程：`image → gray → blurred → edges → roi_edges → HoughLinesP → 方向筛选 → 排序截取 → preview`

本文件按提问顺序整理第四课的四次问答，以及由两次真实报错引出的诊断讨论。

---

## 问题 1：`cv::HoughLinesP(hough_input, lines, 1, CV_PI / 180, 25, 30, 20);` 是什么意思？

这是**概率霍夫变换检测线段**：把 `hough_input` 里散落的白色边缘像素组织成一条条**有端点的线段**，结果写进 `lines`。

```cpp
void cv::HoughLinesP(InputArray image, OutputArray lines,
                     double rho, double theta, int threshold,
                     double minLineLength = 0, double maxLineGap = 0);
```

### 七个参数逐个说

| 位置 | 实参 | 类型/含义 |
| --- | --- | --- |
| image | `hough_input` | 8 位单通道二值图（CV_8UC1），**非零像素 = 边缘点**。传彩色或灰度图会直接抛异常 |
| lines | `lines` | 输出容器，必须是 `std::vector<cv::Vec4i>`。每个 `Vec4i` 是 4 个 int：`(x1, y1, x2, y2)`，整数像素坐标 |
| rho | `1` | 距离参数 ρ 的**分辨率**，1 像素：累加器在"到原点距离"方向上的格子宽度 |
| theta | `CV_PI / 180` | 角度参数 θ 的分辨率，单位**弧度**，这里 = 1°：角度方向上的格子宽度 |
| threshold | `25` | 累加器投票阈值：票数**严格大于** 25 的 (ρ, θ) 格子才算一条线。不是灰度值，也不是 Canny 阈值 |
| minLineLength | `30` | 线段最短长度（像素），短于 30 的候选段丢弃 |
| maxLineGap | `20` | 沿同一直线，两个边缘点距离 ≤ 20 像素才允许连成一段，超过就断开 |

### 为什么这样就能"检测"出直线

直线写成法式：`rho = x * cos(theta) + y * sin(theta)`（ρ 是原点到直线的**有符号垂直距离**，θ 是**法线**方向角，不是线段与 x 轴的夹角）。

- 固定一个边缘点 (x, y)，它在 (ρ, θ) 参数空间里对应**一条正弦曲线**。
- 多个**共线**的点，曲线交于同一个 (ρ, θ)。
- 实现上就是一张二维计数表（累加器），每个点的曲线所过之处各自 +1；格子里的数就是支持这条直线的点数。

`rho=1, theta=1°` 决定的正是这张表的**格子大小** → 决定精度、内存和速度。格子越小越准也越慢。

### `HoughLinesP` 与 `cv::HoughLines` 的区别

| | `HoughLines` | `HoughLinesP` |
| --- | --- | --- |
| 投票 | 所有边缘点 | **随机采样**一部分点 |
| 输出 | `(rho, theta)`，**无限长的直线** | 两个端点 `(x1, y1, x2, y2)`，**有限线段** |
| 点复用 | 每个点都参与 | 归入某条线段后从工作区**抹掉**，不再重复投票 |
| 速度 | 慢 | 快得多，且自带长度信息 |

那个"抹掉"就是讲义要求 `hough_input = roi_edges.clone()` 的原因：输入缓冲区会被就地改写（实现的历史包袱，参数名义上仍是 `InputArray`），所以留一份 `roi_edges` 用于保存和对照。

### 这组具体取值意味着什么

- **`threshold=25` 与 `minLineLength=30` 是一组偏严格的搭配。** 二者量纲不同（票数 vs 几何长度），但互相牵制：一条长 30 像素的直线大约只能贡献 30 个边缘点（斜走 8 连通会略多几个），而阈值要求"超过 25 票"——已经逼近 30 长线段能达到的上限，差不多得是**几乎连续**的 30 像素直线才能过闸。这就是样例上结果不丰富、短噪声基本进不来的原因。
- **`maxLineGap` 最能改变行为**：虚线标线、被遮挡或过曝断掉的标线靠它连成一整段；调太大会把两条无关的线硬接在一起。
- 端点顺序**不保证**从左到右、从下到上；一条有宽度的标线会产生**左右两条边缘**；同一条物理标线可能被检测为多段——这正是后续需要斜率筛选、排序和数量上限的原因。

### 两个容易踩的坑

1. `lines` 是**输出参数**（按引用传出）。新声明的空 vector 没问题；在循环里反复调用则每次都要 `lines.clear()`，否则结果不断累加。
2. 容器类型写错（如 `vector<Vec4f>`）不会编译报错，但运行时会抛类型不匹配异常——`OutputArray` 在运行时校验类型。

### 调参实验（改一个数看一次）

| 改动 | 预期现象 |
| --- | --- |
| `threshold` 25 → 60 | 线段数明显减少，只剩最"扎实"的几条 |
| `maxLineGap` 20 → 3 | 虚线被切成许多小段，线段总数反而变多 |
| `minLineLength` 30 → 100 | 短噪声全消失，也可能一条都不剩 |
| `rho` 1 → 5，`theta` → `CV_PI / 36`（5°） | 变快变粗，端点位置与角度误差变大 |

---

## 问题 2：`std::hypot` 是什么东西？

`std::hypot` 是 `<cmath>` 里的数学函数：**给定直角三角形的两条直角边，返回斜边长**——也就是点 (x, y) 到原点的欧氏距离。名字来自 hypotenuse（斜边）。

出现位置是 `src/line_utils.cpp` 的 `line_length`（`rank_and_limit` 的排序比较器靠它取长度）：

```cpp
double line_length(const cv::Vec4i& segment) {
    const double dx = segment[2] - segment[0];
    const double dy = segment[3] - segment[1];
    return std::hypot(dx, dy);
}
```

线段的两端点差 `dx`、`dy` 是两条直角边，`hypot` 给出斜边，也就是**线段长度**：

```text
length = hypot(x2 - x1, y2 - y1) = √( (x2-x1)² + (y2-y1)² )
```

### 为什么不直接写 `std::sqrt(dx*dx + dy*dy)`

数学等价，`std::hypot` 存在的理由是数值上的两个陷阱和一个边界约定：

**① 溢出**

```cpp
std::hypot(1e200, 1e200);              // ≈ 1.414e200，正确
std::sqrt(1e200*1e200 + 1e200*1e200);  // 1e200*1e200 = inf → 结果是 inf，错
```

**② 下溢 / 精度损失**

```cpp
std::hypot(1e-200, 1e-200);              // ≈ 1.414e-200，正确
std::sqrt(1e-200*1e-200 + 1e-200*1e-200); // 两边下溢成 0 → 结果是 0，错
```

标准库实现会先把**绝对值大的那个**提出来做公因子（形如 `|x| * sqrt(1 + (y/x)²)`），避免中间量越界。

**③ 边界值**：`hypot(inf, NaN)` 规定返回 `+inf`，手写版本会得到 `NaN`。

### 重载与返回类型

| 实参类型 | 返回类型 |
| --- | --- |
| `float, float` | `float` |
| `double, double` | `double` |
| `long double, long double` | `long double` |
| 其它算数类型（int 等） | 按 **double** 处理 |
| `hypot(x, y, z)`（C++17 起） | 三维版本 |

`v[2] - v[0]` 是 `int`，走最后一条"其它类型"的重载：**先提升为 double 再算**，返回值是 `double`。所以 `auto` 推导出 `double`，同时顺带回避了"两个 int 相乘溢出"的问题。

注意写成 `std::hypot`（`<cmath>` 已包含），不写 `std::` 前缀则会命中 C 的全局 `hypot`。

### 四种等价写法怎么选

| 写法 | 说明 |
| --- | --- |
| `std::hypot(dx, dy)` | 二维距离，最稳，语义最清楚 |
| `std::sqrt(dx*dx + dy*dy)` | 等价，但可能溢出/下溢 |
| `dx*dx + dy*dy` | 只需比较大小就够用（平方根单调递增） |
| `cv::norm(p1 - p2)` | OpenCV 风格，Point 相减后求模，返回 `double` |

**排序比较器其实可以省掉开方**：比较 `a > b` 等价于比较 `a² > b²`，比 `dx*dx + dy*dy` 就够。原项目没这么做，是因为线段只有几十条，可读性更重要。判断标准：**需要长度的真实数值时才开方；只比较长短时用平方和**。

---

## 问题 3：`#pragma once` 不加会怎么样？

`#pragma once` 是写给**预处理器**的指令：**这个头文件在同一个编译单元里只展开一次**。不加，重复展开就带来重复定义。

### 实测（用原项目的 `include/visionflow/core/config.hpp`）

```bash
# ① 保留 #pragma once，同一个头文件包含两次
printf '#include "visionflow/core/config.hpp"\n#include "visionflow/core/config.hpp"\n' \
  | g++ -E -I include -x c++ - | grep -c "struct Config"
→ 1

# ② 去掉第 1 行 #pragma once，把内容展开两次
{ sed '1d' include/visionflow/core/config.hpp; sed '1d' include/visionflow/core/config.hpp; } \
  | g++ -fsyntax-only -I include -x c++ -
→ <stdin>:22:8: error: redefinition of 'struct visionflow::Config'
  <stdin>:5:8:  note: previous definition of 'struct visionflow::Config'
```

两处细节：

1. **只有 `struct Config` 报错**：同一份内容里的函数声明（`void validate() const;` 等）重复两遍毫无怨言——C++ 允许**重复声明**，不允许重复**定义**。类/结构体写了 `{}` 主体就是定义。
2. 错误发生在**编译期**，不需要运行。

（同样的实验换成 `include/processor.hpp` 也可以，只须补 `-isystem /usr/include/opencv4`。）

### 为什么真实项目里一定会被包含两次

`#include` 在预处理阶段就是**纯文本复制粘贴**，没有智能去重。原项目里已存在这样的链路：

- `src/net/server.cpp` → `net/server.hpp` → `core/config.hpp`
- `src/net/server.cpp` → `vision/processor.hpp` → `core/config.hpp`（又一遍）

所以 `server.cpp` 展开时 `config.hpp` 的内容被插了两遍。今天没事，全靠每个文件开头那行 `#pragma once`——原项目 8 个头文件全都带着，不是巧合，是必需。

### 关键区分：两种"重复定义"，`#pragma once` 只治其中一种

| | 场景 | 报错时机 | 能治吗 |
| --- | --- | --- | --- |
| **A. 同一编译单元重复展开** | 一个 .cpp 直接/间接包含了同一头文件两次 | **编译错误** `redefinition of ...` | ✅ 能 |
| **B. 多个编译单元各有一份定义** | 头文件里写了**非 inline** 的函数/变量定义，被 10 个 .cpp 各包含一次 | **链接错误** `multiple definition of ...` | ❌ 不能 |

B 类拦不住的原因：每个 .cpp 里它确实只展开了一次，guard 完全没意见——但链接器会看到 10 份同名符号。治 B 类要靠 `inline` / `static` / `constexpr` / C++17 `inline` 变量，或把定义放进 .cpp。

这就是"头文件放声明和类型，.cpp 放定义"这条惯例的由来。

### `#pragma once` 与宏保护对比

```cpp
#ifndef VISIONFLOW_CORE_CONFIG_HPP
#define VISIONFLOW_CORE_CONFIG_HPP
// ...内容...
#endif
```

| | `#pragma once` | `#ifndef` 宏保护 |
| --- | --- | --- |
| 是否标准 | **非标准**，编译器扩展（GCC/Clang/MSVC 全支持，事实上通用） | 标准 C++，任何编译器 |
| 判重依据 | **文件身份**（路径/inode） | **宏名**是否已定义 |
| 速度 | 略快：第二次连文件都不用打开 | 每次都要打开、扫到 `#endif` |
| 失效场景 | 同一文件经两条不同路径/符号链接被看到；不认识的编译器只**警告**然后忽略 | 宏名撞车，或 `#define` 写错/漏写 → **整个头文件被静默跳过**，报错处远离现场 |

`#pragma` 本身不是标准语法，是留给编译器定义扩展的入口；`once` 是各家都实现了的那一个。现代项目普遍统一用 `#pragma once`，两者叠加也不会出错。

### 结论

即使当前只被包含一次也要加：成本一行，收益是消掉一整类编译错误。只被包含一次是**当下的巧合**，包含关系会随重构变化。附带好处：guard 也顺带打破了循环包含（a.h ↔ b.h）导致的无限递归展开。位置放在文件第一个有效行、任何 `include` 之前。

---

## 问题 4：构建失败——`src/line_utils.cpp` 报 `‘cv’ does not name a type`

### 诊断

`src/line_utils.cpp` **一条 `#include` 都没有**。它用了 `cv::Vec4i`、`std::vector`、`std::size_t`、`std::sort`、`std::hypot`，但对这个编译单元来说这些名字从未出现过。

### 为什么同一个函数在 `vision_cli.cpp` 里能编过

两个文件的编译命令**参数完全相同**（`-I../include -isystem /usr/include/opencv4 -std=c++17`），唯一差别是源文件名。结果是：

```text
[1/3] Building ... src/line_utils.cpp.o   FAILED
[2/3] Building ... apps/vision_cli.cpp.o  成功
```

差别只在于 `apps/vision_cli.cpp` 写了 `#include "line_utils.hpp"`，而 `src/line_utils.cpp` 没写。头文件里的 `<cstddef>`、`<vector>`、`<opencv2/core.hpp>` 只在**它被展开的地方**生效。这是讲义 04-3b 第 3 节的实证：**每个 cpp 连同它展开的头文件各自形成一个编译单元，彼此独立**。

### 错误信息怎么读

```text
error: ‘cv’ does not name a type                      ← 第一句，真正的错误
error: expected unqualified-id before ‘&’ token       ← 连锁反应
error: expected ‘)’ before ‘&’ token                  ← 连锁反应
error: variable or field ‘rank_and_limit’ declared void
error: ‘lines’ was not declared in this scope
```

1. **只有第一条是病因。** 编译器不认识 `cv::Vec4i`，只好当别的东西解析，越猜越乱。
2. **从第一条错误开始修，改完重新编译。** 后面绝大多数报错会自己消失。
3. **报错位置不等于错误原因。** `‘lines’ was not declared in this scope` 指向第 7 行的参数名，那行完全没写错，错的是左边的类型没人认识。
4. GCC 会把答案直接写出来（fix-it hint）：

```text
note: ‘std::vector’ is defined in header ‘<vector>’; did you forget to ‘#include <vector>’?
  +++ |+#include <vector>
```

### 修法

在 `src/line_utils.cpp` 最前面加：

```cpp
#include "line_utils.hpp"

#include <algorithm>
#include <cmath>
```

实测结论（`g++ -fsyntax-only`）：

- **最小可行是一行**：只加 `#include "line_utils.hpp"` 就能编过，因为 `<opencv2/core.hpp>` 恰好间接带了 `<algorithm>` 和 `<cmath>`。
- **规范做法是写三行**：用到 `std::sort` 就自己 include `<algorithm>`，用到 `std::hypot` 就自己 include `<cmath>`。今天靠间接包含能过，明天 OpenCV 换版本内部实现改了，报错的却是自己这份无辜的代码。这原则叫 **include what you use（谁使用，谁包含）**。

三个细节：

- **自己的头文件放第一位**（空一行再放标准库）：不只是排版。若 `line_utils.hpp` 自己漏了某个 include，会在**这个文件的第一行**炸出来，等于让编译器顺手检查头文件是否自给自足。
- **头文件和 cpp 是同一条规则**：`line_utils.hpp` 里的三行恰好对应它声明所需的类型——`<cstddef>`→`std::size_t`、`<vector>`→`std::vector`、`<opencv2/core.hpp>`→`cv::Vec4i`。
- 包含自己的头文件的价值不只是"能编过"（不包含也能编过），更在于让编译器拿**声明**和**定义**对照检查一致性：签名改了只改一处时，错误立刻停在实现文件里，而不是变成调用处莫名其妙的报错或链接错误。
- 引号 `"line_utils.hpp"` 先去本项目路径（CMake 里 `-I include`）找，尖括号 `<vector>` 去系统路径找。自己项目的头文件用引号。

### 修好之后的预期

`CMakeLists.txt` 已把 `src/line_utils.cpp` 列进 `add_executable`，所以补上 include 后应一路编译链接成功。反过来，若忘了把它加进 `add_executable`，会看到另一类错误：`undefined reference to 'line_length(...)'`——链接阶段才出现，即讲义 04-3b 表格里的最后一行。

---

## 问题 5：为什么不把 `process_lane_lines` 直接放进 `ProcessResult` 结构体定义中？

因为**成员函数隐含一个 `this` 参数**，而 `process_lane_lines` 根本不需要一个已存在的 `ProcessResult` 对象——它做的事是"根据图像**造出**一个 `ProcessResult`"。做成成员函数，等于要求"先有一个结果，才能算出这个结果"。

### C++ Core Guidelines 的两条规则

> **C.4**：Make a function a member only if it needs direct access to the representation of a class.
> （只有当函数需要直接访问某个类的内部表示时，才做成成员函数。）
>
> **C.5**：Place helper functions in the same namespace as the class they support.
> （辅助函数放在它所服务的类所在的命名空间里。）

把声明放在和 `ProcessResult` 同一个头文件（将来的同一个 namespace）里，正是 C.5 的做法——但"放在同一命名空间"和"塞进结构体"是两件不同的事。

### 实测：成员函数没有对象就调不了

```cpp
struct R {
    int width = 0;
    R process(const cv::Mat& m) const;   // 假设改成成员函数
};

void f(const cv::Mat& m) {
    R r = R::process(m);        // ① 想不用对象直接调用
}
void g(const cv::Mat& m) {
    R r = R{}.process(m);       // ② 只能先造一个空壳对象
}
```

```text
error: cannot call member function ‘R R::process(const cv::Mat&) const’ without object
```

② 那种写法能编过，但语义荒谬：

```cpp
ProcessResult result = ProcessResult{}.process_lane_lines(image);  // 造个空壳，用完就扔
ProcessResult shell;                                               // 或者
const ProcessResult result = shell.process_lane_lines(image);      // shell 不参与任何计算
```

`shell` 从头到尾只为了满足"成员函数必须有对象"这条语法要求。**一个函数如果凭空多出一个用不上的 `this`，就是被放错了地方。**

### 对照：同一个项目里的 `json()` 就该是成员函数

原项目 `include/visionflow/vision/processor.hpp`：

```cpp
struct ProcessResult {
    ...
    nlohmann::json json(std::uint64_t request_id) const;   // 成员函数：它读的是 this 的字段
};
```

判据一目了然：

| 函数需要什么 | 正确写法 | 本课/原项目例子 |
| --- | --- | --- |
| 读懂/修改**某个已有对象**的状态 | 成员函数 | `ProcessResult::json()` |
| 只根据参数算出**新东西**，不碰已有对象 | 命名空间下的自由函数 | `process_lane_lines(image)` |
| 是该类型的"构造方式"，或要访问 private 字段 | **static** 成员函数 | `static ProcessResult from_image(const cv::Mat&)` → `ProcessResult::from_image(img)` |
| 算法自带状态 / 需要运行时多态 | 给它**自己的类型** | `IProcessor` + `create_processor` |

第四行正是本项目的演进方向：正式版里"处理"成为**独立对象**的行为，因为它有状态（`name`、`max_lines`）、有多实现、要运行时选择；而 `ProcessResult` 始终是纯数据快照。**行为应该挂在"执行者"上，不是挂在"结果"上。**

### 写成自由函数白拿的四个好处

1. **复用和测试**：不必先构造对象。以后单测可直接 `process_lane_lines(img)`。
2. **头文件依赖更小**（实际工程里最值钱）：成员函数**必须声明在类定义内部**，而类定义不能拆开写。算法一旦成为成员，就和数据结构永久绑死：改算法签名 → 改结构体定义 → 所有包含该头文件的 .cpp 全部重编译。自由函数让数据定义纹丝不动，算法单独演进。
3. **泛化能力**：C++ 标准库正是"算法与容器分离"——`std::sort(v.begin(), v.end())` 而不是 `v.sort()`。自由函数能同时服务 vector、数组和自定义容器；成员函数只能属于某一个类型。（`std::string::find` 是成员函数，因为它与字符串的内部表示强耦合——两边都对得上。）
4. **归属清晰**：`process_lane_lines` 属于"处理流程"这个概念，`ProcessResult` 是它的输出类型。

### 前瞻：namespace 与接口稳定性

本课 `include/processor.hpp` 目前所有名字都在全局作用域。等加上 `namespace visionflow { ... }`，数据与算法会被一起包进去，那时"自由函数"的完整意义才体现出来：**同一命名空间里的数据 + 算法**，而不是硬塞进一个类型。全局作用域下还有个现实风险：`line_length`、`rank_and_limit` 这类通用名字迟早会和别的库撞车。

---

## 问题 6：能不能使用 `new`？

语法上完全合法——`IProcessor* processor = new LaneLinesProcessor();` 能编过、能跑。但真正该问的是**"谁负责 delete"**。

### 把 main 改成裸 new 需要付出什么

```cpp
IProcessor* processor = new LaneLinesProcessor();     // ← 第 19 行变成这样
...
if (!cv::imwrite("build/edges.png", result.edges)) {
    std::cerr << "Cannot save image: build/edges.png\n";
    delete processor;                                  // ← 每个出口都要补
    return 1;
}
```

`main` 里有 **5 个 `return 1`** 和一个 `catch`——每一处都是潜在的泄漏路径。catch 那里还有更阴的坑：异常可能**发生在 new 之前**（如 `imread` 抛异常），此时 `delete` 一个未初始化的指针是未定义行为。要写对，指针必须提到 `try` 外、初始化为 `nullptr`、再在 catch 里 delete——于是这一行会"污染"整个函数的结构。

这就是 C++ Core Guidelines **R.11「避免显式 new/delete」** 想拦的事：裸指针的所有权靠人脑记忆维护，而人脑在"第 5 个 return 之前忘了 delete"上并不可靠。

### 当前的写法为什么更好

```cpp
LaneLinesProcessor lane_processor;
IProcessor& processor = lane_processor;
```

- **销毁自动且异常安全**：栈对象离开作用域必定析构；中途抛异常，栈展开时照样析构。不需要任何 `delete`，也不存在"某条路径漏了"。
- **多态照样工作**（最常被误解的一点）：**虚函数分派不需要堆**，虚表指针是对象的一部分，栈对象同样拥有。通过基类引用调用 `processor.process(image)`，执行的就是 `LaneLinesProcessor::process`——已跑通的输出即为证据，全程没有一次 `new`。
- **引用是零开销的**：它本质上只是个别名，不复制对象、不引入额外间接层。
- `IProcessor& processor = lane_processor;` 这一行买到的还有一件事：**main 剩余代码只依赖接口**。明天把第 18 行换成 `EdgesProcessor edges_processor;`，第 20 行往下一行都不用改；若直接写 `lane_processor.process(image)`，实现类型就泄漏到了调用点。（两种写法运行结果相同，区别在改动的影响范围。）

### 但"永远不用 new"也是错的

| 需求 | 怎么做 |
| --- | --- |
| 具体类型/数量**运行时**才知道（如按配置 `"lane_lines"` / `"edges"` 选实现） | `std::unique_ptr<IProcessor>` + `std::make_unique<...>` |
| 对象生命周期要**超出**当前作用域（存成员变量、放进容器、返回给调用者） | 同上，`unique_ptr` 表达独占所有权 |
| 编译期类型已知、活到作用域结束（**当前情况**） | 栈上对象 + 基类引用 |

需要动态分配时的现代写法：

```cpp
std::unique_ptr<IProcessor> processor = std::make_unique<LaneLinesProcessor>();
const ProcessResult result = processor->process(image);   // 注意箭头 ->
// 任何出口都不需要 delete，离开作用域自动释放
```

`unique_ptr` 是**零开销抽象**：大小就是一个指针（默认删除器不占空间），析构里就是一句 `delete`，没有引用计数、没有运行时开销——只是把"必须 delete"交给类型系统强制保证。

这个形态原项目已经有了：

```cpp
std::unique_ptr<IProcessor> create_processor(const std::string& name, int max_lines = 128);
```

工厂内部用 `make_unique` 造对象，**调用者从头到尾不需要 delete**。将来按名字选处理器时，只需把第 18-19 行换成 `auto processor = create_processor(name, 3);`，main 其余部分一行不改。

### 顺带解释 `virtual ~IProcessor() = default;`

这行是必需的，而且**正是为堆分配准备的**：若将来有人 `delete` 一个指向派生对象的基类指针，没有虚析构函数就是未定义行为（只调用基类析构，派生类资源不释放）。用栈对象时它其实用不上（栈对象按静态类型析构），但接口必须这么写——因为你无法禁止别人堆分配。这也是个信号：**接口要假设使用者可能用堆，但你自己不必用。**

### 结论

- **别用裸 new**：会拖进手工维护多条释放路径的泥潭，异常路径上无法可靠做对。
- **当前两行就是最佳解**：编译期类型已知 + 生命周期等于作用域 → 栈对象 + 基类引用。
- **要动态分配时**：`std::make_unique` + `std::unique_ptr`，永远不写 `delete`；把 `new` 关在工厂函数内部。
- 传参表达"只是借用、不拥有"用 `IProcessor&`（允许为空时用 `IProcessor*`）——裸指针在 C++ 里的正统含义是**非拥有的观察者**，而不是"需要我自己 delete 的东西"。

---

## 附：本课关键代码与术语速查

| 位置 | 代码 | 作用 |
| --- | --- | --- |
| `src/processor.cpp:62` | `cv::HoughLinesP(hough_input, candidates, 1, CV_PI/180, 25, 30, 20)` | 概率霍夫，1px / 1° / 25 票 / 最短 30 / 最大间隙 20 |
| `src/line_utils.cpp:10` | `std::hypot(dx, dy)` | 线段长度，数值稳健的 √(dx²+dy²) |
| `src/line_utils.cpp` 首部 | `#include "line_utils.hpp"` + `<algorithm>` + `<cmath>` | 自己的头文件放首位；用到什么就包含什么 |
| `include/*.hpp` 首行 | `#pragma once` | 同一编译单元内只展开一次 |
| `include/processor.hpp:27-31` | `class IProcessor { virtual ProcessResult process(...) = 0; }` | 行为挂在执行者身上 |
| `apps/vision_cli.cpp:18-19` | 栈对象 + `IProcessor&` | 多态不需要堆；自动析构 |
| 原项目 `create_processor` | 返回 `std::unique_ptr<IProcessor>` | 运行时选实现时 `new` 只出现在工厂内部 |

| 术语 | 含义 |
| --- | --- |
| 编译单元 | 一个 .cpp 连同它展开的全部头文件；各单元独立编译 |
| 声明 vs 定义 | 声明可以重复，定义不能在同一编译单元重复；类体、函数体是定义 |
| 累加器（accumulator） | 霍夫变换的二维计数表，格子大小由 rho、theta 决定 |
| 概率霍夫 | 随机采样投票、用过的点不再复用，输出有限线段端点 |
| include what you use | 谁使用谁包含，不依赖别人间接带来的头文件 |
| 自由函数 vs 成员函数 | 是否需要 `this`：需要就读写对象状态，不需要就放命名空间 |
| RAII / 栈对象 | 资源生命周期绑定作用域，析构自动执行，异常路径也安全 |
| 零开销抽象 | `unique_ptr` 这类，编译后不引入额外运行时成本 |
| 虚析构函数 | 通过基类指针删除派生对象时必需，否则是未定义行为 |
