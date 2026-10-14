# Linux 主目录迁移

- 来源：`/mnt/c/Users/86180/Desktop/c++/C++项目`。
- 当前项目：`/home/rikanshin/VisionFlow`。
- Windows 来源保留为备份；后续在 Linux 当前项目开发。
- 源码、配置、Git 元数据与已有报告已复制并逐文件 SHA-256 校验。
- 未复制旧 build 和 Python 缓存；CMake 含绝对路径，必须在新位置重新构建。
- 历史报告中的旧输入路径保留原样，代表当时实际运行环境。
- 原验证记录 docs/validation.md 是迁移前的记录。迁移后结果见本文件后续记录。

## Post-migration verification

- Debug and Release rebuilt from scratch at the new location; CTest 3/3 passed in each.
- Invoked smoke_test.sh from /home/rikanshin without changing to the project directory; passed.
- Recording/replay: 30 frames. Benchmark: 200/200 successful requests.
- New demo: /home/rikanshin/VisionFlow/reports/demo-20260917-200007
- Build cache paths checked: /home/rikanshin/VisionFlow.
