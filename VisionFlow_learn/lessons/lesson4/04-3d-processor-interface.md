# 第四模块·第三步 D：亲手实现处理器接口

## 完成记录

学生已实现接口与道路派生类，成功构建运行，结果与之前一致；正确说明调用经过派生类 process 和 process_lane_lines。继续学习 [第二种处理器](04-3e-edges-processor.md)。

## 本课安排

学生了解类、继承和多态的概念，但尚未实际实现。本课通过小步改动练习，主要代码和构建运行由学生完成。已有源文件和依赖足够，CMake 无需修改。

本课只添加一个道路处理器，并通过基类引用调用。下一步再加入另一种处理器；智能指针和工厂在询问熟悉程度后另行学习。

## 1. 这次要建立的调用关系

```text
main 持有 LaneLinesProcessor 对象
    ↓ 绑定到 IProcessor&
processor.process(image)
    ↓ 调用派生类覆盖的函数
LaneLinesProcessor::process(image)
    ↓ 复用现有算法
process_lane_lines(image)
    ↓
ProcessResult
```

接口约定处理器接收什么、返回什么；具体算法由派生类提供。本课保留已有普通函数作为算法实现，通过成员函数转调它，便于把注意力放在调用关系上。

## 2. 在头文件中添加两个类

打开 `include/processor.hpp`，保留 ProcessResult 和 process_lane_lines 的原有声明，在它们之后添加：

```cpp
class IProcessor {
public:
    virtual ~IProcessor() = default;
    virtual ProcessResult process(const cv::Mat& image) = 0;
};

class LaneLinesProcessor : public IProcessor {
public:
    ProcessResult process(const cv::Mat& image) override;
};
```

这里具体落实了几个概念：

- `public:`：允许调用者使用后面的成员。
- `virtual ... = 0`：声明纯虚函数，使 IProcessor 成为抽象类。它规定处理入口，但这里不提供处理算法；不能直接创建 IProcessor 对象。
- `: public IProcessor`：公开继承，允许在需要 IProcessor 引用的位置使用 LaneLinesProcessor 对象。
- `override`：要求编译器检查该成员确实覆盖了基类虚函数。它不负责开启多态；基类的 virtual 才建立了虚调用机制。
- `virtual ~IProcessor() = default`：提供虚析构函数和编译器生成的默认实现。以后通过基类指针销毁派生对象时，需要正确调用派生类析构。本课使用局部对象，不需要手动 delete。

注意区分函数参数上的 `const cv::Mat&` 与成员函数末尾的 const；它们不是同一件事。当前基类和派生类声明保持完全一致即可。

## 3. 在实现文件中定义成员函数

打开 `src/processor.cpp`，在已有 process_lane_lines 函数完整结束之后添加：

```cpp
ProcessResult LaneLinesProcessor::process(const cv::Mat& image) {
    return process_lane_lines(image);
}
```

`LaneLinesProcessor::` 指明这是该类的成员函数定义。若遗漏这个限定，写成单独的 `process(...)`，就会变成另一个普通函数，不能补上头文件声明的那个成员实现。

类外定义无需再次写 virtual 或 override；它们已写在类内声明中。

## 4. 修改主程序的调用

在 `apps/vision_cli.cpp` 中，将：

```cpp
const ProcessResult result = process_lane_lines(image);
```

替换成：

```cpp
LaneLinesProcessor lane_processor;
IProcessor& processor = lane_processor;
const ProcessResult result = processor.process(image);
```

第一行创建实际执行算法的对象。

第二行给这个对象绑定一个基类引用，没有复制或新建一个 IProcessor 对象。lane_processor 在 main 的当前作用域内一直存在，所以这个引用在调用期间有效。

第三行通过基类接口调用虚函数。虽然引用的声明类型是 IProcessor&，它引用的实际对象是 LaneLinesProcessor，最终执行的是 LaneLinesProcessor::process。

这也是后面替换算法的基础：调用点只使用 process 和 ProcessResult，不必跟着具体算法实现一起修改。当前创建对象的那一行仍然明确依赖具体类型，以后学习工厂时再处理创建部分。

不要写 `IProcessor processor = lane_processor;`：本例抽象类不能实例化，而且我们要引用已有对象。也不要写 `const LaneLinesProcessor lane_processor;`：当前 process 是非 const 成员函数，这样会限制本课的调用方式。

原有打印统计、遍历 result.lines、保存图片和错误处理全部保留。

## 5. 由你构建并运行

```bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
cmake --build build
./build/vision_cli
echo $?
```

仍使用同一份道路算法，预期统计和端点与前一小步相同：全图边缘 2095，ROI 边缘 1088，候选与方向筛选后各 8 条，最终 3 条，长度为 257.002、255.601、254.201。正常退出码应为 0。

这些是学生此前提供的对照结果，不代表老师代为运行本次改动。

## 6. 结合自己的代码说明调用过程

发来运行输出，并说明：

- `processor` 这个引用的类型是什么，它引用的实际对象是什么类型？
- `processor.process(image)` 最终进入哪个类的成员函数？

这一步清楚后，再加入第二种处理器，实际观察相同接口如何产生不同处理行为。
