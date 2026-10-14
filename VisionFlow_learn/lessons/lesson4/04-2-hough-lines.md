# 第四模块·第二步：检测并绘制线段

## 本课目标与分工

把 ROI 边缘图中的部分白色像素组织成线段，打印线段端点并将其画在原图副本上。本课先看未经方向筛选的检测结果；理解后再学习筛选，不提前重构处理器接口。

老师维护讲义和辅助文件，你修改 `apps/vision_cli.cpp` 并自行构建、运行。现有头文件、素材和 CMake 依赖已足够，不需要新增配置。

## 1. 边缘像素如何变成线段

边缘图只说明每个像素是不是边缘，没有记录线段的起点和终点。

Hough（霍夫）变换的基本思路是投票：一个边缘点可能位于多条直线上；如果多个点支持同一条直线，这条线就得到较多票数。实现时将直线参数划分为小格，用累加器记录票数。

直线常用下面的形式表示：

```text
rho = x * cos(theta) + y * sin(theta)
```

`rho` 是直线的有符号法向距离参数，`theta` 是法线方向的角度，不是线段本身与 x 轴的夹角。本课先理解“多个点共同支持某条直线”，不要求推导公式。

`cv::HoughLinesP` 使用概率霍夫方法检测有限长度的线段，输出两个端点：`(x1, y1, x2, y2)`。它不是直接返回上面的 rho 和 theta，也不负责判断线段是否属于车道。

## 2. 添加检测代码

在打印 `ROI edge pixels` 之后、现有图片保存代码之前，加入：

```cpp
cv::Mat hough_input = roi_edges.clone();
std::vector<cv::Vec4i> lines;
cv::HoughLinesP(hough_input, lines, 1, CV_PI / 180, 25, 30, 20);

std::cout << "Line segments: " << lines.size() << "\n";
```

`std::vector` 存放数量可变的一组元素；`cv::Vec4i` 存放四个整数。因此 `lines` 中每个元素就是一条线段的四个端点坐标。

`clone()` 复制像素数据，得到独立图像。本机 OpenCV 接口文档注明 Hough 输入图像可能被修改，因此用副本检测，保留 `roi_edges` 用于保存和对照。

参数顺序如下：

| 参数 | 本课取值 | 含义 |
| --- | --- | --- |
| image | hough_input | 8 位、单通道二值边缘图 |
| lines | lines | 用于接收线段端点的容器 |
| rho | 1 | 距离参数分辨率，单位为像素 |
| theta | CV_PI / 180 | 角度参数分辨率，单位为弧度；这里是 1 度 |
| threshold | 25 | 累加器投票阈值，控制所需的支持强度 |
| minLineLength | 30 | 最小线段长度参数，用于排除太短的线段 |
| maxLineGap | 20 | 沿同一直线连接边缘点时允许的最大间隙，单位为像素 |

这里的 25 是投票阈值，不是 Canny 的梯度阈值，更不是像素灰度值。距离分辨率 1 也不是绘制时的线宽。

本课针对 640×360 样例先使用固定最小长度 30。原项目使用 `max(15, h / 12)` 随高度调整，本课高度下同样得到 30。当前沿用学生的模糊参数和 100/200 Canny 阈值，不要求与原项目完整处理结果相同。

## 3. 遍历线段并画回原图

紧接着添加：

```cpp
cv::Mat preview = image.clone();
for (const auto& segment : lines) {
    std::cout << "(" << segment[0] << ", " << segment[1] << ") -> ("
              << segment[2] << ", " << segment[3] << ")\n";

    cv::line(preview,
             cv::Point(segment[0], segment[1]),
             cv::Point(segment[2], segment[3]),
             cv::Scalar(0, 255, 0), 2);
}
```

`for (... : lines)` 是范围 for 循环，依次访问容器中的每条线段：

- `auto` 让编译器推导元素类型，这里是 `cv::Vec4i`。
- `&` 表示通过引用访问原元素，避免复制。
- `const` 表示不通过这个引用修改线段。
- 索引从 0 开始：`segment[0]`、`[1]` 是起点 x、y，`[2]`、`[3]` 是终点 x、y。

`cv::line` 在 `preview` 上画线，`cv::Scalar(0, 255, 0)` 按 BGR 指定绿色，最后的 2 是绘制线宽，单位为像素。

ROI 没有改变图像尺寸或坐标，因此检测坐标可以直接用在原图上，不需要额外偏移。端点顺序不保证从左到右或从下到上，不能依靠端点顺序判断左右车道。

### 为什么要 clone

```cpp
cv::Mat preview = image.clone(); // 独立的像素副本
```

如果改成 `cv::Mat preview = image;`，两个 Mat 会共享同一份像素数据。在这里画线会同时改变通过 `image` 看到的内容。使用 `clone()` 后，绘制只改变副本。

## 4. 保存预览

保留现有的边缘图和掩膜保存代码，在成功路径的 `return 0;` 之前添加：

```cpp
if (!cv::imwrite("build/lines_preview.png", preview)) {
    std::cerr << "Cannot save image: build/lines_preview.png\n";
    return 1;
}
std::cout << "Saved: build/lines_preview.png\n";
```

新代码仍放在已有 `try` 内，保留 `catch`。

## 5. 由你构建并观察

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake --build build
./build/vision_cli
echo $?
```

打开 `build/lines_preview.png`，观察绿色线段是否覆盖了道路标线的部分轮廓，并与 `build/roi_edges.png` 对照。

线段数不等于真实车道线数量：一条有宽度的标线可能产生两侧边缘，一条物理标线也可能被检测为多段。此时不要求结果恰好为两条，也不预先指定线段数。没有检测到线段时容器为空，循环不执行，保存的预览仍是原图副本；先反馈实际结果，再讨论参数。

## 6. 理解检查

请发来线段数量、部分端点坐标、退出码以及预览观察结果，再回答：

1. `lines.size()` 统计的是边缘像素数量，还是线段数量？
2. 为什么使用 `image.clone()` 生成预览，而不是直接写 `preview = image`？

这些内容清楚之后，再学习按方向筛除不合适的线段。
