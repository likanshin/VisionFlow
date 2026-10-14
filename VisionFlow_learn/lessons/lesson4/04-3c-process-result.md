# 第四模块·第三步 C：用结构体返回处理结果

## 本课状态

学生已声明掌握 struct、成员默认初始化及函数返回结构体，因此按约定跳过教学与问答，由老师直接完成重构。老师未代为执行课程实践；学生现已提供成功构建和运行输出，统计及选中的 3 条线段与重构前一致。

上一小步的头源文件拆分已由学生构建并运行，贴出的输出仍为 2095 个全图边缘、1088 个 ROI 边缘、8 条候选、8 条方向合格线段、最终 3 条线段。以下作为代码改动记录与复习参考。

## 改动范围

- `include/processor.hpp`：定义 ProcessResult，声明 process_lane_lines。
- `src/processor.cpp`：集中执行已有的灰度、模糊、Canny、ROI、Hough、方向筛选、排序截取和预览绘制。
- `apps/vision_cli.cpp`：读取图片，调用处理函数，打印结果并保存图片。
- `CMakeLists.txt`：将 src/processor.cpp 加入可执行目标。
- line_utils 的现有声明与实现继续复用。

```text
main 读取图片
    ↓
process_lane_lines(image)
    ↓ 返回 ProcessResult
main 打印统计与端点，保存结果图片
```

本次是已有流程的封装，没有引入继承、虚函数或工厂。相关新知识点会先询问熟悉程度。

## 返回数据

| 字段 | 含义 |
| --- | --- |
| width / height / channels | 输入图片的宽、高、通道数 |
| edge_pixels | 全图边缘像素数量 |
| roi_edge_pixels | ROI 内边缘像素数量 |
| candidate_line_count | Hough 检测出的线段数量 |
| kept_line_count | 方向筛选后、数量截取前的线段数量 |
| lines | 最终保留的线段端点 |
| edges / roi_mask / roi_edges | 用于教学观察的中间图像 |
| preview | 在原图独立副本上绘制线段的预览 |

数值成员有默认初值；vector 和 Mat 使用默认构造。不同阶段的数量单独记录，防止截短 lines 后丢失“筛选前后数量”的观测信息。

接口是：

```cpp
ProcessResult process_lane_lines(const cv::Mat& image);
```

输入约定为非空的 8 位 BGR 图像，当前调用方通过 imread 的彩色读取模式和 empty 检查满足这一条件。处理函数只负责计算；文件路径、终端输出和图片保存仍由 main 管理。

函数内部的 Mat 持有受引用计数管理的图像数据，按值返回结构体可以维持结果数据的生命周期。以后再复制 ProcessResult 时，其 Mat 成员依然遵循 OpenCV 默认共享像素数据的语义，不等于自动深拷贝所有图片。需要独立绘制的 preview 仍明确使用 image.clone()。

## 保留的处理参数与输出

- 高斯模糊：5×5，sigma=0。
- Canny：100/200。
- ROI：原梯形四顶点比例。
- Hough：1、CV_PI/180、25、30、20。
- 方向门槛：0.35；最大线段数：3。
- 继续保存 edges.png、roi_mask.png、roi_edges.png、lines_preview.png 到 build。

完整处理成功后，再由 main 输出结果。若处理中途抛出异常，会直接进入已有的 OpenCV 异常处理分支，部分统计不会像旧版那样在计算过程中提前打印。

## 由学生验证

在学习工程目录执行：

```bash
cmake --build build
./build/vision_cli
echo $?
```

固定素材、参数及环境下，预期仍为：

```text
Edge pixels: 2095
ROI edge pixels: 1088
Line segments: 8
Kept line segments: 8
Selected line segments: 3
(82, 358) -> (287, 203) length=257.002
(352, 202) -> (556, 356) length=255.601
(355, 205) -> (558, 358) length=254.201
```

查看生成图片并与之前结果对照，正常退出码应为 0。上面的数字来自学生此前的运行记录，不代表老师已经运行验证本次重构。


## 补充讨论：为什么结果用 struct

本项目的 ProcessResult 用于保存并传递一组处理数据，因此采用公开成员的 struct；处理器 class 则负责提供处理行为。C++ 的 struct 与 class 都支持成员函数、访问控制、继承和多态，主要语言区别是默认成员访问和默认继承权限。

如果以后需要强制维护结果不变量，例如禁止外部随意修改、确保尺寸与图像一致，可以改用私有成员和受控构造。仅更换 class 关键字，或给所有字段加上可任意赋值的 setter，并不会自动保证这些约束。
