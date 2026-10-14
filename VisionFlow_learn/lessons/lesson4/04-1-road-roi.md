# 第四模块·第一步：用 ROI 保留道路区域

## 本课目标与分工

把全图的边缘限制在一个预先指定的梯形道路区域中。本模块后续再学习 Hough 线段检测和处理器接口；本课只实现 ROI。

老师维护讲义、素材及构建配置；你修改主要代码，亲自构建、运行并观察结果。本课使用已有素材与 OpenCV 模块，不需要修改 CMake，也无需新增素材。

## 1. ROI 是什么

ROI 是 Region of Interest，即“感兴趣区域”。我们假设本示例中的道路主要位于画面下方，用一个梯形限制后续检测范围，减少区域外背景的干扰。

这是一项人为的几何假设，不是程序已经识别出道路。区域内仍可能存在非车道边缘；真实道路也可能超出这个区域。

本课通过掩膜将区域外像素置零，图像宽高和坐标都保持不变，不是裁出一张更小的图。

## 2. 图像坐标与梯形

图像左上角是 `(0, 0)`；x 向右增大，y 向下增大。宽为 w、高为 h 时，最后一列是 `w - 1`，最后一行是 `h - 1`。

沿用原项目 `src/vision/processor.cpp` 的四个顶点，按梯形边界顺序排列：

| 顶点 | x | y |
| --- | --- | --- |
| 左下 | w / 10 | h - 1 |
| 左上 | w * 45 / 100 | h * 55 / 100 |
| 右上 | w * 55 / 100 | h * 55 / 100 |
| 右下 | w * 9 / 10 | h - 1 |

对于 640×360 图片，四个点是 `(64, 359)`、`(288, 198)`、`(352, 198)`、`(576, 359)`。使用宽高比例，让区域能随图像尺寸变化；整数运算会舍去小数部分。

## 3. 掩膜是什么

掩膜 `mask` 是一张与边缘图同尺寸的单通道图片：希望保留的区域填 255，其余位置填 0。

我们使用逐位与运算：

```text
edges 与 mask → roi_edges
```

8 位像素中，255 的二进制是 `11111111`，0 是 `00000000`。因此任意像素值与 255 做逐位与，保持原值；与 0 做逐位与，得到 0。

| edges 像素 | mask 像素 | 输出 |
| --- | --- | --- |
| 255 | 255（区域内） | 255 |
| 0 | 255（区域内） | 0 |
| 255 | 0（区域外） | 0 |
| 0 | 0（区域外） | 0 |

所以 ROI 只保留已有的区域内边缘，不会把整个梯形变成白色边缘。

## 4. 由你添加代码

在 `apps/vision_cli.cpp` 的头文件区域添加：

```cpp
#include <vector>
```

在已有的 Canny 调用之后、保存图片之前添加以下代码。保留原来的边缘计数和原图输出。本课保持当前 Canny 的 100/200 阈值不变，只观察 ROI 的影响。

```cpp
const int w = edges.cols;
const int h = edges.rows;

cv::Mat mask = cv::Mat::zeros(edges.size(), CV_8UC1);
std::vector<cv::Point> polygon{
    {w / 10, h - 1},
    {w * 45 / 100, h * 55 / 100},
    {w * 55 / 100, h * 55 / 100},
    {w * 9 / 10, h - 1}
};
cv::fillConvexPoly(mask, polygon, cv::Scalar(255));

cv::Mat roi_edges;
cv::bitwise_and(edges, mask, roi_edges);

std::cout << "ROI edge pixels: " << cv::countNonZero(roi_edges) << "\n";
```

逐项理解：

- `const int`：值设定后不再修改的整数，本课用于记录图像宽高。
- `cv::Mat::zeros`：创建所有像素为 0 的矩阵；`edges.size()` 提供宽高。
- `CV_8UC1`：每个分量为 8 位无符号整数，取值 0～255，且只有一个通道。
- `std::vector<cv::Point>`：存放二维坐标点的容器；这里用初始化列表放入四个点。
- `fillConvexPoly`：填充凸多边形。我们的梯形是凸多边形，顶点要沿边界排列。
- `cv::Scalar(255)`：指定填充值；对本课单通道掩膜而言就是白色。
- `bitwise_and`：对两个同尺寸、同类型图像的对应像素逐位与，写入第三个参数。

保留现有的 `build/edges.png` 保存逻辑。然后在 `return 0;` 之前追加以下保存代码，以便同时观察掩膜和处理结果：

```cpp
if (!cv::imwrite("build/roi_mask.png", mask)) {
    std::cerr << "Cannot save image: build/roi_mask.png\n";
    return 1;
}
if (!cv::imwrite("build/roi_edges.png", roi_edges)) {
    std::cerr << "Cannot save image: build/roi_edges.png\n";
    return 1;
}
std::cout << "Saved: build/roi_mask.png and build/roi_edges.png\n";
```

上述新代码仍然位于已有的 `try` 内。保留原有 `catch`，处理 OpenCV 可能抛出的异常。

## 5. 由你构建并观察

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake --build build
./build/vision_cli
echo $?
```

分别打开：

- `build/edges.png`：全图边缘。
- `build/roi_mask.png`：黑底白色梯形。
- `build/roi_edges.png`：仅保留梯形内原有边缘，区域外为黑色。

三个文件尺寸都应与原图相同。ROI 边缘数应小于或等于原来的边缘数，具体数量以实际运行为准。重复运行会覆盖这些练习输出。

## 6. 理解检查

请发来全图边缘数、ROI 边缘数、退出码及图片观察结果，并回答：

1. 梯形内部原本为黑色、不是边缘的像素，与白色掩膜运算后会变成白色吗？为什么？
2. 保留 ROI 之后，图片宽高是否改变？如果不改变，被排除的区域变成了什么？

确认你理解后，再进入 4.2 线段检测。
