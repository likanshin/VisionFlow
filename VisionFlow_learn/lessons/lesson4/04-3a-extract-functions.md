# 第四模块·第三步 A：先提取普通函数

## 本课执行记录

2026-09-24：学生声明已掌握本课知识点，要求跳过教学并由老师完成。老师已按下文方案提取两个普通函数，并将 main 中的排序、截取逻辑替换为函数调用；没有执行构建或运行，待学生验证。下文保留为复习参考，无需再完成本课问答。

## 本课目标与分工

排序与数量限制已经完成，学生理解了先排序再截取，以及 vector 的 size 与 capacity 区别。

当前 main 包含读图、边缘检测、ROI、线段检测、筛选、排序和保存。接下来逐步整理代码结构，本课只提取“计算长度”和“排序并截取”两个普通函数。

先在同一个 cpp 文件内完成，不需要修改 CMake 或新增源文件。主要代码由你修改，构建和运行也由你执行；老师维护讲义。

## 1. 为什么提取函数

把有明确职责的一段代码放进函数后，调用位置可以直接表达意图：

```cpp
rank_and_limit(kept_lines, max_lines);
```

阅读 main 时，只需要知道这里在“排序并限制数量”；具体步骤集中在函数定义中。其他调用者以后也能复用这些操作。

这次是重构：调整代码组织方式，保持现有处理行为。继续使用相同图片、方向门槛 0.35 和最大线段数 3。

## 2. 在 main 前定义两个函数

在所有 include 之后、`int main()` 之前添加以下代码。不要把普通函数定义放进 main 内部。

```cpp
double line_length(const cv::Vec4i& segment) {
    const double dx = segment[2] - segment[0];
    const double dy = segment[3] - segment[1];
    return std::hypot(dx, dy);
}

void rank_and_limit(std::vector<cv::Vec4i>& lines,
                    std::size_t max_lines) {
    std::sort(lines.begin(), lines.end(),
              [](const cv::Vec4i& a, const cv::Vec4i& b) {
                  return line_length(a) > line_length(b);
              });

    if (lines.size() > max_lines) {
        lines.resize(max_lines);
    }
}
```

### 返回值与参数

`double line_length(...)` 表示函数返回一个 double 类型的长度；`return` 把计算结果交给调用者。

`void rank_and_limit(...)` 表示函数不返回值。它通过引用参数直接修改调用者提供的容器，所以不需要另行返回一个新容器。

`max_lines` 是按值传入的整数。本例只读取这个数量上限，复制这个小整数即可。

### const 引用和可修改引用

| 参数形式 | 本课含义 |
| --- | --- |
| `const cv::Vec4i& segment` | 引用一条已有线段，只读取，不通过这个引用修改 |
| `std::vector<cv::Vec4i>& lines` | 引用调用者的容器，允许排序和删除其中的元素 |
| `std::vector<cv::Vec4i> lines` | 按值接收；若传入 kept_lines 这样的左值，会复制容器及其线段元素 |

调用 `rank_and_limit(kept_lines, max_lines)` 时，参数名 lines 指的是调用者的 kept_lines，不是 main 中另一个名叫 lines 的原始检测容器。名字可以不同，传入哪个对象才决定操作哪个对象。

对于本课的 vector<Vec4i>，按值复制会得到独立的线段元素；不要和之前 cv::Mat 的默认浅拷贝行为混淆。

### 为什么比较 lambda 的捕获列表变成空的

以前 line_length 是 main 内部的局部 lambda 变量，需要通过 `[&line_length]` 捕获。

现在 line_length 是在文件作用域定义的普通函数，比较 lambda 可以直接调用它，所以写 `[]`。捕获列表用于使用外层局部变量，并不是调用所有函数都必须捕获。

普通函数放在 main 前面，是为了让编译器在使用时已经见过它的定义。下一小课再学习通过头文件提供声明、把定义放入独立 cpp。

## 3. 缩短 main 中的排序部分

在 main 中，删除上一课添加的以下旧代码：

- 局部的 `const auto line_length = ...;` 定义；
- 对 kept_lines 的 `std::sort(...)` 调用；
- 对 kept_lines 的 resize 判断。

把原来这一整段排序、截取逻辑替换为：

```cpp
const std::size_t max_lines = 3;
rank_and_limit(kept_lines, max_lines);

std::cout << "Selected line segments: " << kept_lines.size() << "\n";
```

注意不要重复保留两份 max_lines 定义或 Selected 输出。绘制循环中的 `line_length(segment)` 不用改写，现在会调用 main 外面的普通函数。

保留 main 中的方向筛选、绘制和图片保存逻辑。本课完成后文件的大致结构是：

```text
include 区域
line_length 函数定义
rank_and_limit 函数定义
main 函数定义
```

## 4. 由你构建并对照

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake --build build
./build/vision_cli
echo $?
```

使用当前相同的图片和参数时，预期仍为：

```text
Line segments: 8
Kept line segments: 8
Selected line segments: 3
(82, 358) -> (287, 203) length=257.002
(352, 202) -> (556, 356) length=255.601
(355, 205) -> (558, 358) length=254.201
```

这些数值来自你上一次实际运行，作为本次重构的对照，不是对所有图片的固定要求。退出码应为 0，预览里的线段应与重构前一致。

## 5. 理解检查

请发来运行结果，并回答：

1. 为什么 rank_and_limit 的容器参数需要 `&`？如果去掉它，对 kept_lines 这样的左值传参，main 中原来的容器还会被排序、截短吗？
2. line_length 的参数为什么可以加 const，而 rank_and_limit 的容器参数不能加 const？

确认理解后，再进行头文件与源文件拆分，不提前引入继承和工厂。
