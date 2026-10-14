# 性能实验

使用 Release，先生成合成数据。服务端通过 `cv::setNumThreads(0)` 关闭 OpenCV 内部并行，应用工作线程由配置指定。

```bash
conda activate visionflow
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DPython3_EXECUTABLE="$CONDA_PREFIX/bin/python"
cmake --build build/release --parallel 4
cmake -E chdir build/release ctest --output-on-failure
python scripts/demo.py --bin-dir build/release/bin
```

演示报告保存于 `reports/demo-日期时间/`：实际配置、服务日志、合成输入、离线结果、录制帧、回放视频、benchmark.json。
压测报告记录客户端平台/Python/OpenCV、JPEG 大小、客户端数、window、目标速率、预热数、时长、成功吞吐和延迟分位数。
成功延迟与所有响应延迟分开统计，丢帧不会被当作成功吞吐。

独立压测已运行的服务：

```bash
conda activate visionflow
python python/benchmark.py --clients 2 --requests 500 --warmup 20 --window 4 --rate 30 --output reports/rate-30.json
python python/benchmark.py --clients 4 --requests 500 --window 8 --output reports/saturation.json
```

`--rate` 是每连接的目标发送速率，window 达到上限时会降低实际速率，不是无界开放环负载发生器。
`--window 1` 即逐请求响应模式。连接/超时错误使压测失败，不输出一个看起来完整的成功报告。
固定输入和配置，重复至少三次后再比较；记录 CPU 型号、内存、WSL 配额和构建模式。

本轮生成的报告只代表该环境下的小型合成测试，不能代表真实道路视频的帧率和准确率。
未进行优化前后对照实验，不宣称性能提升。CPU/RSS 可用 `pidstat`、`ps` 或 `/proc/PID/status` 另行采样；
未将这些外部指标伪装成已自动测量。
