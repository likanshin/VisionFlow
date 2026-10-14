# VisionFlow：C++ 网络视觉处理与回放系统

从设计草案实现的 **C++17 / Linux / OpenCV** 工程。Python 读取视频并发送 JPEG，C++ 通过 TCP 接收、并行处理、返回 JSON，客户端支持显示、记录和离线回放。

原始设计全文保留在 [docs/design.md](docs/design.md)。其中的规划不代表全部已经验收，当前能力以本 README 为准。

## 已实现

- `vision_cli`：本地 JPEG 处理，输出 JSON 和标注/边缘预览图片。
- `vision_server`：epoll 多客户端、24 字节 VFLW 协议、拆包/连续消息、超时、部分收发。
- `edges` / `lane_lines`：Canny 边缘、道路 ROI + Hough 线段，算法工厂与独立线程实例。
- 有界队列、线程池、每连接未完成额度、字节预算、实时淘汰通知和离线背压。
- JPEG 解码前尺寸检查、JSON 配置校验、结构化日志、统计、有序停止。
- `vision_supervisor`：启动子进程、转发停止信号、超时终止、回收并传播退出状态。
- Python 视频客户端、JPEG + JSONL 记录、回放导出、多客户端压测、合成样例生成。
- CMake、CTest、Debug/Release/Sanitizer 开关、Linux CI 配置、单元和集成测试。

完整服务要求 Linux。本机验证环境：**WSL2 Ubuntu 20.04 / GCC 9.4 / CMake 3.16 / OpenCV 4.2 / Python 3.8**。
Windows 原生可构建不依赖图像库的核心测试；网络服务和监管程序在 WSL 中运行。

## 快速开始

以下命令在 **WSL / Linux 终端**、项目根目录执行。当前项目的 WSL 路径：

```bash
cd ~/VisionFlow
```

项目现位于 `/home/rikanshin/VisionFlow`。Windows 原目录仅作为迁移前备份。

无需切换目录，在任意 WSL 目录都可以运行一键构建、测试与演示：

```bash
conda activate visionflow
bash ~/VisionFlow/scripts/smoke_test.sh
```

从任意 PowerShell 目录直接进入项目：

```powershell
wsl -d Ubuntu-20.04 --cd /home/rikanshin/VisionFlow
```

### 1. 依赖与专用环境（本机已配置）

C++ 编译和系统库由 Linux 提供：

```bash
sudo apt-get update
sudo apt-get install -y g++ cmake ninja-build pkg-config libopencv-dev nlohmann-json3-dev
```

Python 使用独立的 Conda `visionflow` 环境，包含 Python 3.11、NumPy 1.26.4 和 OpenCV 4.11.0.86。
环境位于 `/home/rikanshin/miniconda3/envs/visionflow`。本机已创建，每次使用前激活即可：

```bash
conda activate visionflow
```

终端前缀应变为 `(visionflow)`。在其他机器上可从项目根目录重建：

```bash
conda env create -f environment.yml
conda activate visionflow
```

一键脚本要求已激活 `visionflow`，测试和演示统一使用其中的解释器，已移除系统 Python 自动回退及 `VISIONFLOW_PYTHON` 覆盖。
`base` 的包、`.bashrc` 和 `.profile` 未作改动；使用完运行 `conda deactivate` 返回原环境。
图形显示需要桌面环境，默认无窗口运行。

也可以直接通过 Python 运行演示，不需要调用 Bash 脚本（需已构建）：

```bash
conda activate visionflow
python ~/VisionFlow/scripts/demo.py --bin-dir ~/VisionFlow/build/debug/bin
```

### 2. 一键构建、测试和演示

```bash
conda activate visionflow
bash scripts/smoke_test.sh
```

构建 Debug，执行测试，生成合成道路视频，处理单图，启动临时端口服务，录制 30 帧，导出回放，完成 200 次测量请求后自动关闭服务。
产物位于 `reports/demo-日期时间/`：

| 文件 | 内容 |
| --- | --- |
| `input/sample.avi`、`input/frame.jpg` | 合成输入 |
| `frame_result.json`、`frame_preview.jpg` | 离线结果和标注图 |
| `recording/results.jsonl`、`recording/frames/` | 网络结果与输入帧 |
| `replay.avi` | 按请求编号排序后的标注回放 |
| `benchmark.json` | 吞吐、成功/错误数量、P50/P95/P99 |
| `server.json`、`server.log` | 实际配置、错误和统计日志 |

### 3. 手动构建和运行

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DPython3_EXECUTABLE="$CONDA_PREFIX/bin/python"
cmake --build build/debug --parallel 4
cmake -E chdir build/debug ctest --output-on-failure
python python/generate_sample.py
./build/debug/bin/vision_server --config configs/server.json
```

另开一个 Linux 终端，在项目根目录运行：

```bash
python python/client.py --video data/sample.avi --record reports/my-recording
python python/replay.py reports/my-recording --output reports/my-replay.avi
./build/debug/bin/vision_cli --input data/frame.jpg --processor edges --output reports/edges.json --preview reports/edges.jpg
```

录制目录必须是新目录，避免覆盖旧记录。有桌面时加 `--display`，按 Q 停止预览。
服务端按 Ctrl+C 或发送 SIGTERM 停止。监管方式：

```bash
./build/debug/bin/vision_supervisor ./build/debug/bin/vision_server --config configs/server.json
```

不要同时在相同端口运行两种启动方式。systemd 部署示例见 `scripts/visionflow.service`，需按安装路径自行配置。

## 配置与行为

默认配置见 [configs/server.json](configs/server.json)。启动时拒绝未知字段、不正确类型和超预算组合。

| 配置 | 默认 | 说明 |
| --- | --- | --- |
| listen_address / port | 127.0.0.1 / 9000 | IPv4 监听地址 |
| processor | lane_lines | 可改为 edges |
| mode | realtime | 满队列淘汰旧任务；offline 暂停读取等待空位 |
| worker_threads / max_clients | 4 / 8 | 工作线程和客户端上限 |
| max_inflight_per_client | 8 | 已接受但未发完响应的请求额度 |
| task_queue_capacity / task_queue_bytes | 32 / 64 MiB | 队列同时受两种限制 |
| max_payload_bytes | 16 MiB | JPEG 消息体上限 |
| max_pixels | 2073600 | 相当于 1920×1080 |
| idle_timeout_ms / io_timeout_ms | 30000 / 10000 | 空闲、部分消息读取和发送停滞超时 |
| memory_budget_bytes | 640 MiB | 缓冲/图像估算预算，不是进程 RSS 硬限制 |

响应可能乱序，Python 按 request_id 匹配。边缘模式返回边缘像素数量，网络不传回边缘图；离线工具可输出边缘预览。
压测：`python python/benchmark.py --clients 2 --requests 100 --window 4`。
`--rate` 设置每连接目标发送速率，`--window 1` 为逐请求响应。

## 目录与文档

- `apps/`：三个 C++ 可执行入口；`include/visionflow/`、`src/`：协议、队列、网络、配置和算法。
- `python/`：客户端、回放、压测、样例生成；`configs/`：配置。
- `tests/`：单元与真实 TCP 集成测试；`scripts/`：构建演示和部署示例。
- `data/`、`reports/`：生成输入和结果，默认忽略，不纳入版本管理。
- [架构与所有权](docs/architecture.md)、[协议](docs/protocol.md)、[调试](docs/debugging.md)、[性能实验](docs/performance.md)、[实际验证记录](docs/validation.md)。

## 当前边界

用于 C++ 工程学习和可信局域网演示。线段不是车道语义识别，弯道、遮挡、光照变化需要另行评估。
未实现 ONNX、动态库插件、TLS/认证、自动重启、独立监管状态管道，也未完成真实道路准确率和优化前后对照实验。
不把合成视频吞吐当成真实场景性能。GDB 崩溃复现、面试材料及阶段 Git 历史应在后续实际开发中积累。
