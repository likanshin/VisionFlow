# 第四模块·第二步补充：按长度排序并限制数量

## 本课目标与分工

方向筛选已通过：0.35 门槛保留 8 条，0.8 门槛保留 0 条；水平线段被丢弃、非零长度的竖直线段保留。

本课继续由你修改主要代码并构建、运行。老师维护讲义和学习进度，不代为执行实践。当前代码中的方向门槛已经恢复为 0.35；实践时保持这个值。

目标：把通过方向筛选的线段按长度从大到小排序，最多保留 3 条用于打印和绘制。3 是便于观察的教学上限，原项目默认最大线段数为 128，后续再用配置统一管理。

## 1. 为什么先排序再截取

Hough 返回顺序不保证最长的线段在最前面。直接保留前三条，得到的是原顺序的前三条，不一定是最长的三条。

```text
方向筛选 → 按长度降序排序 → 限制数量 → 打印并绘制
```

较长线段提供更长的几何片段，但不因此一定是真实车道。本课规则也不保证左右两侧各保留一条，不会自动合并重复或相近线段。

## 2. 用端点计算长度

沿用 dx、dy 的定义，两点间距离为：

```text
length = sqrt(dx * dx + dy * dy)
```

标准库 `std::hypot(dx, dy)` 计算这个长度，单位是像素。

在头文件区域添加：

```cpp
#include <algorithm>
#include <cstddef>
```

`algorithm` 提供排序；`cstddef` 提供 `std::size_t`。已有的 `cmath` 提供 `std::hypot`。

在输出 `Kept line segments` 之后、创建 `preview` 之前添加下面的完整代码：

```cpp
const auto line_length = [](const cv::Vec4i& segment) {
    const double dx = segment[2] - segment[0];
    const double dy = segment[3] - segment[1];
    return std::hypot(dx, dy);
};

std::sort(kept_lines.begin(), kept_lines.end(),
          [&line_length](const cv::Vec4i& a, const cv::Vec4i& b) {
              return line_length(a) > line_length(b);
          });

const std::size_t max_lines = 3;
if (kept_lines.size() > max_lines) {
    kept_lines.resize(max_lines);
}

std::cout << "Selected line segments: " << kept_lines.size() << "\n";
```

保留前面的 `Kept line segments` 输出：它统计方向筛选之后、截取之前的数量。新的 `Selected line segments` 统计最终数量。

## 3. 认识 lambda 与排序规则

### 定义一个小函数

```cpp
const auto line_length = [](const cv::Vec4i& segment) {
    // ...
};
```

这是 lambda 表达式，可以理解为就地定义一个小函数对象，再用变量 `line_length` 保存它。`auto` 让编译器推导这个对象的类型，调用时写 `line_length(segment)`。

- `[]`：不捕获外层局部变量。参数 segment 已提供计算所需的数据。
- `(const cv::Vec4i& segment)`：通过只读引用接收一条线段。
- `{ ... }`：计算并返回长度。
- 整个变量定义最后仍需分号。

### 提供比较规则

`std::sort` 根据比较函数安排顺序。比较函数返回 true，表示 a 应排在 b 前面。

```cpp
return line_length(a) > line_length(b);
```

所以较长的 a 排在前面，得到降序。如果使用 `<` 则会变成短的在前。

`[&line_length]` 表示通过引用使用外层的 `line_length` 函数对象。排序在这里立即完成，这个对象在调用期间一直存在。

`begin()` 指向第一个元素，`end()` 指向最后一个元素之后的位置；排序范围包含 begin，不包含 end。

比较关系必须严格，不能把 `>` 随意改成 `>=`。长度相等时，本课比较函数返回 false；`std::sort` 不保证等长线段的原有次序。原项目额外用端点坐标打破等长平局，后续与原实现对齐时再补充；本课先掌握按长度排序。

## 4. 为什么 resize 前要判断

`resize(n)` 是把容器大小调整为 n，而不是仅表示“最多保留 n 个”。

- 当前大小大于 n：删除尾部多余元素。
- 当前大小小于 n：增加元素。

我们只想截短，不想补出额外线段，因此必须先判断 `size() > max_lines`。

注意区分 `size()`（实际元素数）与 `capacity()`（现有分配空间可容纳的元素数）。例如 size=8、capacity=8 时，resize(3) 后 size=3、capacity 仍为 8；缩小元素数不会自动缩小容量。size=2、capacity=8 时，resize(3) 会新增一个元素，而容量仍为 8。`std::size_t` 是常用于表示大小的无符号整数类型，与这里 `size()` 返回值的用途一致。

## 5. 打印长度，帮助观察排序

找到已有绘制循环中的端点输出，将它调整为：

```cpp
std::cout << "(" << segment[0] << ", " << segment[1] << ") -> ("
          << segment[2] << ", " << segment[3] << ")"
          << " length=" << line_length(segment) << "\n";
```

绘制循环继续遍历 `kept_lines`，绘制与保存的其余代码不用修改。截取改变的是容器中的线段数量，不会改变已有的全图边缘图或 ROI 边缘图。

## 6. 由你运行与观察

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake --build build
./build/vision_cli
echo $?
```

若检测结果保持不变，预期原始线段数为 8，方向筛选后为 8，最终选择 3 条。打印的三个长度应从大到小排列，退出码应为 0。

打开 `build/lines_preview.png` 观察保留线段。线段可能相互重叠，因此图上视觉可分辨的绿色线条数量未必等于容器中的数量；以打印结果为准。

## 7. 理解检查

请发来运行输出并回答：

1. 为什么先排序再截取，而不是先截取再排序？
2. 如果通过方向筛选的线段只有 2 条，上限为 3，为什么不能直接无条件调用 `resize(3)`？

确认这一步理解后，再开始把图像处理代码整理成独立接口。
