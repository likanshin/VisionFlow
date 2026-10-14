# 调试和验证

本项目测试使用真实 C++ 子进程和 TCP，不依赖外部图片或服务。单元测试不用 `assert`，Release 下同样执行校验。

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel 4
cmake -E chdir build/debug ctest --output-on-failure
```

内存与未定义行为：

```bash
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVISIONFLOW_ENABLE_ASAN=ON -DVISIONFLOW_ENABLE_UBSAN=ON
cmake --build build/asan --parallel 4
cmake -E chdir build/asan ctest --output-on-failure
```

并发核心可以独立验证，不依赖 OpenCV：

```bash
cmake -S . -B build/tsan -G Ninja -DVISIONFLOW_CORE_ONLY=ON -DVISIONFLOW_ENABLE_TSAN=ON -DBUILD_TESTING=ON
cmake --build build/tsan --parallel 4
cmake -E chdir build/tsan ctest --output-on-failure
```

ASan 与 TSan 不可同时启用。WSL 内核/地址布局可能限制 TSan，若运行时初始化失败，不能据此宣称没有数据竞争。

需要交互调试时安装 GDB，然后：

```bash
gdb --args build/debug/bin/vision_server --config configs/server.json
```

使用 `break main`、`run`、`bt`、`info threads`、`thread apply all bt`。队列等待点在模板头中的条件变量，
正常空队列阻塞是预期行为；关闭时应唤醒所有生产者和消费者。
测试包含关闭时唤醒、满队列淘汰、线程任务抛异常后继续处理、无效协议后其他连接仍可用等场景。

后续真实问题记录请包含：复现命令、输入版本、编译模式、现象、调用栈、根因、修复、复测。
这里提供复现方式，不伪造未发生的崩溃与 GDB 调用栈。

## 本轮实际定位：OpenCV / TBB 退出时泄漏报告

Ubuntu 20.04 / OpenCV 4.2 / GCC 9 的 ASan 首轮报告 `libtbb.so.2+0x2766f` 中的分配未释放。
用独立程序排除了协议、队列和业务算法的影响：

```bash
g++ -fsanitize=address -fno-pie -no-pie -g tests/diagnostics/opencv_threads_probe.cpp -I/usr/include/opencv4 -lopencv_imgproc -lopencv_core -o build/opencv_threads_probe
./build/opencv_threads_probe 1
./build/opencv_threads_probe 0
```

本机第一条运行报告 792 字节 / 3 个分配泄漏，第二条退出码 0、无泄漏报告。
因此服务和 CLI 改为 `cv::setNumThreads(0)`，完全关闭库内并行，由应用线程池负责并发。
没有屏蔽 `libtbb`，没有设置 `detect_leaks=0`。监管程序也移除了不需要的 OpenCV 链接依赖。

旧 ASan 在本机还出现过启动阶段反复 `DEADLYSIGNAL`。本轮为独立 sanitizer 构建加入以下参数后复测：

```bash
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DVISIONFLOW_ENABLE_ASAN=ON -DVISIONFLOW_ENABLE_UBSAN=ON -DCMAKE_CXX_FLAGS=-fno-pie -DCMAKE_EXE_LINKER_FLAGS=-no-pie
cmake --build build/asan --parallel 4
cmake -E chdir build/asan ctest --output-on-failure
```

这是本地 sanitizer 构建的布局调整，普通 Debug/Release 没有改变；未修改系统 ASLR 设置。
上游也有 [高随机地址布局下旧 sanitizer 启动异常的报告](https://github.com/google/sanitizers/issues/1614)，
与本机症状一致，但这里没有把它当作已通过调试器证明的唯一根因。
