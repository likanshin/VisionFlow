# 第三课：生成边缘图

## 本课分工与目标

老师已维护本讲义，并在 `CMakeLists.txt` 中加入 OpenCV 的 `imgproc` 模块。你负责修改 `apps/vision_cli.cpp`，然后亲自配置、构建、运行和观察结果。老师不代为执行课程实践。

本课使用现有的 `samples/frame.jpg`，不需要新素材。目标是保存边缘图并统计边缘像素数；完成理解检查后才进入下一模块。

## 1. 处理流程

```text
彩色图 image → 灰度图 gray → 高斯模糊 blurred → Canny 边缘图 edges
```

彩色图每个像素有 B、G、R 三个颜色分量；灰度图用一个分量表示明暗。转为灰度后，我们可以关注亮度变化。

边缘通常出现在亮度变化明显的地方，例如道路与白色标线的交界。噪声也可能造成局部变化，因此先用高斯模糊减弱细小干扰。平滑也可能削弱细节，后续可通过实验观察这个取舍。

## 2. 了解已经准备好的构建配置

老师已将查找依赖的语句改成：

```cmake
find_package(OpenCV 4 REQUIRED COMPONENTS core imgcodecs imgproc)
```

`imgproc` 提供颜色转换、滤波和边缘检测。已有的 `${OpenCV_LIBS}` 链接配置会包含找到的模块。你无需再次修改这行，但仍需亲自执行后面的配置和构建命令。

## 3. 你来修改主要代码

### 添加头文件

在 `apps/vision_cli.cpp` 顶部的头文件区域增加：

```cpp
#include <opencv2/imgproc.hpp>
```

### 添加图像处理逻辑

确认输入路径为 `samples/frame.jpg`。在 `if (image.empty()) { ... }` 的整个检查块之后、仍位于 `try` 内，添加：

```cpp
cv::Mat gray;
cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);

cv::Mat blurred;
cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

cv::Mat edges;
cv::Canny(blurred, edges, 50, 150);

std::cout << "Edge pixels: " << cv::countNonZero(edges) << "\n";
```

第一行创建暂时为空的矩阵，随后 `cvtColor` 将转换结果写入其中。后面的 `blurred` 和 `edges` 采用同样的输入、输出方式。

| 代码 | 含义 |
| --- | --- |
| `cvtColor(image, gray, cv::COLOR_BGR2GRAY)` | 将 BGR 彩色图转成灰度图 |
| `GaussianBlur(gray, blurred, ...)` | 对灰度图进行高斯平滑 |
| `cv::Size(5, 5)` | 使用宽、高各为 5 个像素的邻域进行加权平滑 |
| `GaussianBlur` 最后的 `0` | 根据核尺寸计算所需的高斯标准差 |
| `Canny(blurred, edges, 50, 150)` | 用低阈值 50、高阈值 150 检测边缘 |
| `countNonZero(edges)` | 统计边缘图中非零像素的个数 |

Canny 的阈值针对亮度变化的强弱，不是像素本身的亮度。Canny 会筛选边缘候选；强候选作为可靠边缘，较弱候选需要通过候选边缘与强边缘相连才可能保留。

本课 Canny 输出是单通道图像：边缘像素为 255（白），其余像素为 0（黑），因此统计非零像素就是统计检测到的边缘像素。

### 保存边缘图

将原来的 `imwrite` 检查和成功提示替换为：

```cpp
if (!cv::imwrite("build/edges.png", edges)) {
    std::cerr << "Cannot save image: build/edges.png\n";
    return 1;
}
std::cout << "Saved: build/edges.png\n";
```

保留随后的 `return 0;` 和 `catch` 错误处理。注意：保存函数的第二个参数是 `edges`，输出路径和提示文字都要对应更新。若保存位置的旧注释只描述图片副本，也请相应调整。

## 4. 由你构建并运行

在 WSL / Linux 终端执行：

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake -S . -B build -G Ninja
cmake --build build
./build/vision_cli
echo $?
```

预期程序输出 `Edge pixels: ...` 和 `Saved: build/edges.png`，正常退出码为 0。具体边缘数量以你的实际结果为准，不预先指定一个数。

打开 `build/edges.png`，与 `samples/frame.jpg` 对照：观察白色线条是否对应道路标线等物体的轮廓。重复运行会覆盖本课的边缘图输出。

## 5. 理解检查

先发来运行输出和你的观察结果，并回答：

> 一块亮度完全均匀的纯白区域，其内部一定会被检测为边缘吗？为什么？

讨论这个问题后，再根据理解情况安排本模块内的后续练习。不提前进入下一模块。

## 6. 已完成的阈值实验与结论

学生已将 Canny 阈值由 50/150 改为 100/200 并自行构建运行，两次报告的边缘像素数均为 2095，退出码为 0。

保持输入、预处理和其他 Canny 参数不变，只提高低、高阈值时，结果集合单调不增：候选像素与强边缘起点都只会减少或不变，不会凭空增加连通路径。因此新边缘集合是旧边缘集合的子集，数量可以减少，也可以保持不变。

如果这些条件满足，且两次数量相同，结合子集关系可推得两次边缘像素位置也相同。但不能断言所有保留像素的梯度都超过新高阈值：较弱候选仍可能通过连通关系保留。

学生已正确回答：在 100/200 阈值下，梯度为 130、通过前序筛选的候选像素，若通过合格候选链连接到梯度为 250 的强边缘则保留；无法连接任何强边缘则丢弃。第三课理解检查通过。
