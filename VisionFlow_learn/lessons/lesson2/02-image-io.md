# 第二课：读取和保存图片

## 目标与边界

读取一张图片，打印宽、高和通道数，再保存为 PNG。完成并理解本课后，才进入边缘检测。

本课是学习用的最小读写流程。原项目最终从 JPEG 字节解码，并在解码前检查尺寸；这些限制会在后续模块补齐。

## 1. 图片文件与内存图像

```text
samples/frame.jpg → imread 解码 → cv::Mat → imwrite 编码 → build/frame_copy.png
```

JPEG 是磁盘上存放图片的一种压缩格式。算法通常需要访问解码后的像素，所以先用 OpenCV 的 `imread` 读取并解码。

OpenCV 是图像处理库；`cv::` 是它的命名空间。`cv::Mat` 是矩阵类型，可以表示图像及其尺寸、像素类型等信息。

```cpp
cv::Mat image = cv::imread("samples/frame.jpg", cv::IMREAD_COLOR);
```

- `cv::Mat`：变量的类型。
- `image`：变量名。
- `=`：这里用右边的读取结果初始化变量。
- `cv::imread(...)`：函数调用，第一个参数是文件路径。
- `cv::IMREAD_COLOR`：按三通道 BGR 彩色图读取。B、G、R 分别表示蓝、绿、红。

把图像理解成一张由像素组成的表：列数是宽度，行数是高度；每个像素有几个分量，就是几个通道。本课读入的每个像素有三个颜色分量。

```cpp
image.cols        // 宽，单位是像素
image.rows        // 高，单位是像素
image.channels()  // 通道数
```

`.` 用来访问对象的成员。`cols`、`rows` 是成员变量，`channels()` 是成员函数，所以后者带括号。

## 2. 为什么先检查读取结果

```cpp
if (image.empty()) {
    std::cerr << "Cannot read image: samples/frame.jpg\n";
    return 1;
}
```

`empty()` 判断矩阵是否为空。找不到文件或无法解码时，`imread` 可能返回空矩阵；此时不应继续保存。

`if` 表示条件为真时执行花括号里的语句。`std::cerr` 是标准错误流，通常也显示在终端，但可以与 `std::cout` 的正常输出分开处理。这里用非零退出码 `1` 报告失败。

## 3. 保存图片

```cpp
if (!cv::imwrite("build/frame_copy.png", image)) {
    std::cerr << "Cannot save image: build/frame_copy.png\n";
    return 1;
}
```

`imwrite` 把矩阵编码成图片文件，扩展名 `.png` 决定这里使用 PNG 格式。返回 `true` 表示成功；`!` 表示逻辑取反，因此这个分支处理返回 `false` 的情况。

这是对解码后图像的重新编码，不是逐字节复制 JPEG 文件。PNG 无损保存本次解码得到的像素，但不能恢复 JPEG 编码时已经损失的信息。

OpenCV 的某些错误也会抛出异常。示例外层的 `try` 执行可能出错的操作；`catch (const cv::Exception& error)` 接住 OpenCV 异常，通过 `error.what()` 输出原因并返回 `1`。先理解它是另一条错误处理路径，异常类型与引用语法以后继续展开。

## 4. 让构建系统找到 OpenCV

本课在 `CMakeLists.txt` 中追加了：

```cmake
find_package(OpenCV 4 REQUIRED COMPONENTS core imgcodecs)
target_include_directories(vision_cli PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(vision_cli PRIVATE ${OpenCV_LIBS})
```

三行依次完成：查找 OpenCV 的基础模块与图片读写模块；告诉编译器到哪里寻找头文件；告诉链接器需要哪些库。

`${...}` 读取 CMake 变量的值。`PRIVATE` 表示这项依赖设置用于当前目标；本课先掌握这一用途。

头文件提供类型和函数声明，链接库提供需要的已编译实现，因此仅添加 `#include` 并不能替代库的链接配置。

当前机器已安装系统 C++ OpenCV，本课无需安装依赖。终端中的 `(visionflow)` 是 Python 环境标识，本课 C++ 使用的 OpenCV 由 CMake 查找。

## 5. 运行

在 WSL / Linux 中执行：

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake -S . -B build -G Ninja
cmake --build build
./build/vision_cli
echo $?
```

程序会输出第一课的问候语、图片宽高、通道数和保存位置。通道数应为 `3`，退出码应为 `0`。查看 `build/frame_copy.png`，应能看到合成道路图片。重复运行会覆盖该输出。

代码里的 `samples/frame.jpg` 相对于程序运行时的工作目录，不是相对于 `.cpp` 所在目录，也不是相对于可执行文件所在目录。因此要先进入 `VisionFlow_learn` 再运行。

## 6. 动手练习

1. 正常运行，记下宽、高、通道数与退出码，并打开保存的图片。
2. 把 `imread` 的路径改为 `samples/not-found.jpg`，重新构建并运行，紧接着执行 `echo $?`。观察读取失败时的提示和退出码。不要删除原素材。
3. 恢复正确路径，再增加一行输出总像素数：`image.cols * image.rows`。预测总像素数后，构建并运行验证。通道数不是像素个数的倍数因子。

练习 2 时，之前生成的 PNG 仍然会留在磁盘上；文件存在不代表本次运行成功，应检查本次输出和退出码。

## 7. 理解检查

请发来练习输出，并回答：

1. `cv::Mat image` 中装的是文件路径、JPEG 压缩文件，还是解码后的图像数据？
2. 为什么必须检查 `image.empty()`？读图失败后应返回 `0` 还是非零值？
3. 从学习目录执行 `./build/vision_cli` 与进入 `build` 后执行 `./vision_cli`，相对路径寻找图片的位置是否相同？为什么？

有不清楚的地方就停在这里讨论；不提前进入第三课。
