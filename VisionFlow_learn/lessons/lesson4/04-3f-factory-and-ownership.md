# 第四模块·第三步 F：工厂函数与独占所有权实践

## 实践记录

学生反馈代码可以运行，并正确解释工厂返回后的所有者是 processor，移动后由 moved_processor 管理和释放对象；所有权理解检查通过。老师只读核对了工厂、统一基类智能指针返回以及标准异常捕获，没有代为执行构建或运行。

当前主程序仍使用 create_processor("666") 测试非法名称；请由学生恢复 edges 或 lane_lines，重新构建运行，以正常版本结束本课。

## 本课安排

学生了解 RAII、unique_ptr、make_unique、移动语义和工厂的概念，但没有实际实现。本课以实践为主，由学生修改主要代码、构建并运行。现有 CMake 不需要调整，老师只维护讲义和进度。

完成目标：用处理器名称创建对象，返回统一的 unique_ptr<IProcessor>，通过指针调用，并观察一次明确的所有权转移。命令行参数属于下一模块，本课暂时在代码中指定名称。

## 1. 声明工厂函数

在 `include/processor.hpp` 的 include 区域添加：

```cpp
#include <memory>
#include <string>
```

在 IProcessor 和两个派生类声明之后、文件末尾添加：

```cpp
std::unique_ptr<IProcessor> create_processor(const std::string& name);
```

返回类型表示调用者将拥有一个处理器对象；通过基类接口使用它，实际对象可以是不同的派生类。工厂成功时返回有效指针，名称非法时抛异常，本课不使用“失败返回空指针”的约定。

## 2. 实现创建逻辑

在 `src/processor.cpp` 顶部添加：

```cpp
#include <stdexcept>
```

在文件末尾添加：

```cpp
std::unique_ptr<IProcessor> create_processor(const std::string& name) {
    if (name == "edges") {
        return std::make_unique<EdgesProcessor>();
    }
    if (name == "lane_lines") {
        return std::make_unique<LaneLinesProcessor>();
    }
    throw std::invalid_argument("unknown processor: " + name);
}
```

make_unique 创建具体对象并立即交给 unique_ptr 管理。这里返回的 unique_ptr<派生类> 可以通过移动转换交给 unique_ptr<IProcessor>，转移所有权，不复制派生对象，也不发生对象切片。

两个 return 已经返回临时对象，无需加 std::move。工厂不返回局部对象的裸地址，实际处理器由智能指针拥有，不会随着工厂函数结束而销毁。

只有当最终拥有它的智能指针被销毁、重置或替换时，相应对象才会被释放。作用域正常结束、return 或异常栈展开都会触发局部智能指针的析构；这不意味着强制终止进程时也一定执行析构。

IProcessor 的虚析构函数现在有实际用途：unique_ptr<IProcessor> 通过基类指针销毁对象时，能够正确执行派生类析构，再执行基类析构。不需要手动 delete。

## 3. 替换主程序的创建和调用

当前主程序使用一个具体类型的局部对象，再绑定基类引用。将这三行替换为：

```cpp
auto processor = create_processor("edges");
const ProcessResult result = processor->process(image);
```

auto 推导出的类型是 std::unique_ptr<IProcessor>。这里改用 `->` 调用被指向对象的成员，而不再用原来的引用表达式 `processor.process(image)`。

打印统计、条件保存 ROI、保存 preview.png 的代码保留。

现在 main 不再写具体处理器类名；名称到具体类型的选择集中在工厂。因为名称仍写在代码中，换名称仍需重新构建，下一模块接入命令行后才可在启动时选择。

## 4. 接住名称错误

当前 main 只有 `catch (const cv::Exception& ...)`，它不会接住新工厂抛出的 std::invalid_argument。

在 `apps/vision_cli.cpp` 顶部添加：

```cpp
#include <exception>
```

保留原有 OpenCV catch，并在它后面、main 的最后一个右花括号之前追加标准异常处理，末尾整体变成：

```cpp
    } catch (const cv::Exception& error) {
        std::cerr << "OpenCV error: " << error.what() << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "vision_cli: " << error.what() << "\n";
        return 1;
    }
}
```

std::invalid_argument 和 cv::Exception 都派生自 std::exception，所以专门的 OpenCV catch 放在前面，通用 catch 放在后面。如果把通用 catch 写在前面，它会先匹配 OpenCV 异常。

## 5. 先验证工厂的三条路径

在学习目录按下面顺序自行修改名称、构建、运行。每次 echo $? 紧跟程序运行：

```bash
cmake --build build
./build/vision_cli
echo $?
```

| 名称 | 预期行为 |
| --- | --- |
| edges | 全图边缘 2095，最终线段 0，正常退出码 0 |
| lane_lines | 全图边缘 2095，ROI 边缘 1088，最终线段 3，正常退出码 0 |
| unknown | 提示 vision_cli: unknown processor: unknown，退出码 1 |

数字基于当前样例和不变的算法参数。名称非法时，不应继续保存图片；磁盘上已有的输出不会因此消失，不能仅凭文件存在判断本次成功。

## 6. 亲自观察移动

完成上面三条路径后，将名称恢复为 edges，再做一次所有权实验。

在主程序头部添加：

```cpp
#include <utility>
```

将创建和调用的两行暂时替换为：

```cpp
auto processor = create_processor("edges");
auto moved_processor = std::move(processor);

std::cout << "Original owner empty: " << (processor == nullptr) << "\n";
std::cout << "New owner valid: " << (moved_processor != nullptr) << "\n";

const ProcessResult result = moved_processor->process(image);
```

重新构建并运行，预期上述两个布尔输出都是 1，而且边缘处理仍正常完成。

std::move 本身只是允许使用移动语义的类型转换；这里真正转移所有权的是初始化 moved_processor 时调用的移动构造函数。被管理的 EdgesProcessor 对象没有因此重新执行构造，也没有被复制。

unique_ptr 在这种移动完成后，源指针 processor 为空。此后不能再写 processor->process(image)，应使用新的拥有者。这个明确的空状态是 unique_ptr 的保证，不要泛化为所有被移动对象都变成空。

如果去掉 std::move，写 `auto moved_processor = processor;`，会尝试复制 unique_ptr，而它禁止复制，因此无法编译。本课无需故意保留这行错误代码。

实验完成后恢复第 3 节的简洁工厂调用，删掉临时的移动状态输出；如果不再使用 std::move，也可以移除新增的 utility include。重新构建和运行，保留可以正常工作的版本。

## 7. 理解检查

请发来工厂三种名称的运行结果，以及移动实验的两行状态输出，并解释：

1. 工厂结束后，为什么返回的处理器对象仍然存在？
2. 执行移动后，processor 和 moved_processor 中谁负责最终释放对象？

完成后，再询问下一模块的新知识点熟悉程度。本课不提前添加命令行解析、配置文件或线程池。
