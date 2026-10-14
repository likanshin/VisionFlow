# 项目专用 Python 环境

当前环境为 Conda `visionflow`，位置：`/home/rikanshin/miniconda3/envs/visionflow`。

- Python 3.11.16
- NumPy 1.26.4
- opencv-python 4.11.0.86（cv2 版本 4.11.0）
- 创建文件：项目根目录 `environment.yml`

## 使用

```bash
conda activate visionflow
python ~/VisionFlow/scripts/demo.py --bin-dir ~/VisionFlow/build/debug/bin
```

终端前缀应显示 `(visionflow)`。完整构建测试入口仍为 `bash ~/VisionFlow/scripts/smoke_test.sh`，但必须先激活项目环境。Bash 是脚本运行所用的 shell，与 Python 虚拟环境不同。

结束时使用 `conda deactivate` 返回之前的环境。

## 原环境与项目设置

本轮没有向 base 安装、升级或卸载包，没有编辑 ~/.bashrc、~/.profile 或全局 Conda 配置。
base 仍保持原有初始修订。此前修复加入的系统 Python 自动回退和 VISIONFLOW_PYTHON 覆盖现已移除。
Debug/Release 的 CMake Python3_EXECUTABLE 已统一指向新环境。
现有系统 C++ 编译器和 OpenCV 开发库继续供 C++ 构建使用，独立于 Python 环境。

## 验证

- 新环境 `python -m pip check`：无依赖冲突。
- Debug 与 Release：CTest 均 3/3 通过，包含 11 个集成测试方法。
- 新环境一键演示：30 帧录制回放、200/200 压测请求成功。
- 记录：`reports/visionflow-env-smoke.log`。
- 演示：`reports/demo-20260917-210755/`。
- 在 base 中误调用一键脚本时会在构建前提示激活 visionflow，不自动切换环境。
- 验证中修正了重连测试的偶发连接额度冲突：该用例配置 16 个客户端额度，生产默认仍为 8。
