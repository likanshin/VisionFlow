# 第四模块·第三步 E：用同一接口切换两种处理器

## 实践记录

学生已实现边缘处理器，提供成功构建后的边缘模式输出：Edge pixels 为 2095，Selected line segments 为 0，只保存 edges.png 和 preview.png。学生指出使用不同派生类，已补充说明虚函数调用取决于实际对象。接下来通过工厂再验证两种模式和错误路径。

## 本课安排

你已实现 IProcessor 和 LaneLinesProcessor，实际构建运行成功，并正确描述调用经过派生类 process 和普通函数 process_lane_lines。

继续练习同一个多态知识点：添加 EdgesProcessor，观察相同接口调用如何执行另一种算法。主要代码和构建运行由你完成，当前 CMake 与头文件依赖足够。

本课使用局部对象和基类引用；智能指针与工厂尚未开始，之后会先询问熟悉程度。

## 1. 两种处理器的结果约定

| 结果 | LaneLinesProcessor | EdgesProcessor |
| --- | --- | --- |
| 宽、高、输入通道数 | 有 | 有 |
| 全图边缘及数量 | 有 | 有 |
| ROI 掩膜、ROI 边缘 | 有 | 空 Mat，表示未计算 |
| 最终线段 | 筛选后的线段 | 空容器 |
| preview | 在原图上画线段 | 显示全图边缘 |

接口的函数签名统一，还需要明确返回数据的含义。边缘模式没有计算 ROI，就不应该为了适配保存代码而制造虚假的 ROI 图片。

## 2. 在头文件添加第二个派生类

在 `include/processor.hpp` 中，现有类定义之后添加：

```cpp
class EdgesProcessor : public IProcessor {
public:
    ProcessResult process(const cv::Mat& image) override;
};
```

## 3. 实现边缘处理器

在 `src/processor.cpp` 末尾添加：

```cpp
ProcessResult EdgesProcessor::process(const cv::Mat& image) {
    ProcessResult result;
    result.width = image.cols;
    result.height = image.rows;
    result.channels = image.channels();

    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);
    cv::Canny(blurred, result.edges, 100, 200);
    result.edge_pixels = cv::countNonZero(result.edges);

    cv::cvtColor(result.edges, result.preview, cv::COLOR_GRAY2BGR);
    return result;
}
```

这些是之前已经写过的步骤。最后将单通道边缘图转换为 BGR 预览，使两种处理器都返回同尺寸、三通道的 preview；它看起来仍然是黑白图，因为每个像素的三个分量相等。

其他字段保持默认值：线段容器为空，ROI 图片为空，相关计数为 0。本课没有运行 ROI 或 Hough。少量相同的边缘提取代码暂时保留，后续整理实现时可以提取公共函数；这里先观察两个实现的切换。

## 4. 让主程序按结果内容输出

### 统计信息

在 `apps/vision_cli.cpp` 中，保留 Width、Height、Channels 和 Edge pixels 的输出。将紧随其后的 ROI edge pixels、Line segments、Kept line segments、Selected line segments 四行替换为：

```cpp
if (!result.roi_edges.empty()) {
    std::cout << "ROI edge pixels: " << result.roi_edge_pixels << "\n";
    std::cout << "Line segments: " << result.candidate_line_count << "\n";
    std::cout << "Kept line segments: " << result.kept_line_count << "\n";
}
std::cout << "Selected line segments: " << result.lines.size() << "\n";
```

现有端点输出循环保留。边缘模式的 lines 为空，所以循环自然不执行。

### 可选的 ROI 图片

保留 edges.png 的保存代码。将原来无条件保存两个 ROI 图片及其成功提示的整段代码替换为：

```cpp
if (!result.roi_mask.empty()) {
    if (!cv::imwrite("build/roi_mask.png", result.roi_mask)) {
        std::cerr << "Cannot save image: build/roi_mask.png\n";
        return 1;
    }
    std::cout << "Saved: build/roi_mask.png\n";
}
if (!result.roi_edges.empty()) {
    if (!cv::imwrite("build/roi_edges.png", result.roi_edges)) {
        std::cerr << "Cannot save image: build/roi_edges.png\n";
        return 1;
    }
    std::cout << "Saved: build/roi_edges.png\n";
}
```

这样主程序依照“结果是否存在”工作，不需要通过具体处理器类型来判断。不能把空 Mat 直接传给 imwrite。

### 统一预览名称

把保存预览处所有 `build/lines_preview.png` 改为 `build/preview.png`，包括 imwrite 参数、错误提示和成功提示。之后两种模式统一查看 preview.png，旧的 lines_preview.png 不会自动更新或删除。

## 5. 用同一个调用点完成两次实验

将主程序中创建与调用处理器的三行整理成：

```cpp
LaneLinesProcessor selected_processor;
IProcessor& processor = selected_processor;
const ProcessResult result = processor.process(image);
```

先构建并运行道路模式：

```bash
cmake --build build
./build/vision_cli
echo $?
```

使用当前素材和参数时，预期仍得到 2095 个全图边缘、1088 个 ROI 边缘、8 条候选和最终 3 条线段。查看 preview.png，应该是道路图片上叠加绿色线段。

接着只修改第一行的具体类型：

```cpp
EdgesProcessor selected_processor;
IProcessor& processor = selected_processor;
const ProcessResult result = processor.process(image);
```

重新构建、运行并立即查看 preview.png。预期全图边缘仍是 2095，Selected line segments 为 0，预览为黑白边缘图；不输出 ROI 统计或保存 ROI 的提示。两次正常退出码都应为 0。

上一次道路模式生成的 ROI 文件仍可能留在磁盘上。它们的存在不代表这次边缘模式执行了 ROI，需要结合本次输出判断。切换模式会覆盖 edges.png 和 preview.png，请在下一次运行之前观察当前预览。

## 6. 这次实践体现的接口价值

改变的是创建对象的具体类型；基类引用和 `processor.process(image)` 调用不变。打印和保存也根据统一的 ProcessResult 处理，不需要写“如果是道路类就调用某函数，否则调用另一个函数”的类型分支。

这并不意味着普通函数不能完成同样的任务。单一算法使用普通函数很直接，少数算法集中用 if/else 选择也合理；接口继承在多个实现需要统一调用时提供另一种组织方式。

当前仍然通过修改源代码选择类型，这是为了专注观察多态。后续工厂负责根据名称创建对象，再逐步接入命令行参数，不在本课提前实现。

## 理解检查

请发来两次运行的输出和预览观察结果，并回答：

> 两次代码中的 processor 都是 IProcessor&，为什么相同的 processor.process(image) 会得到不同类型的预览图？
