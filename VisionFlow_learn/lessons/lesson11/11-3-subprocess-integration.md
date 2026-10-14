# 11-3：用 Python 管理子进程，完成端到端集成验证

你的熟悉程度已记录为“完全不熟悉，需要从头讲”。本课从 subprocess 的基本角色开始，最终写一份能够自动串起现有程序的验证脚本。

11-2 的五点已由老师直接解释，并按你“进入下一环节”的要求收尾；两张预览的可视观察没有反馈，不倒填为已验证。11-3 不要求补跑旧命令，而是把现有程序组织成一次新的、可重复的集成实验。

主要文件由你亲手新建：`/home/rikanshin/VisionFlow/VisionFlow_learn/python/integration_check.py`。老师只维护讲义和进度，不创建主要源码、不配置、构建、运行或测试。本课不修改 CMake，也不安装新依赖。

## 1. 目标：让程序自己检查各环节是否接得上

之前你依次操作多个终端，并阅读输出。集成验证把这些动作组织成一个有明确成功条件的流程：

~~~text
integration_check.py（父进程）
    │
    ├─ 启动 vision_pool_server ─────────────┐
    │                                      │
    ├─ 等待本次服务端日志报告监听           │ 持续运行
    │                                      │
    ├─ 启动 video_client.py ── 五次请求 ────┤
    │      └─ 等待退出，要求返回码 0        │
    │                                      │
    ├─ 向服务端发 SIGTERM，等待并回收 ──────┘
    │
    ├─ 启动 video_replay.py（此时服务端已退出）
    │      └─ 等待退出，要求返回码 0
    │
    ├─ 检查新 JSONL、回放视频与六张 PNG
    │
    └─ 写 report.json，返回 0
~~~

一轮成功必须同时满足：

- 五帧客户端正常退出，新的 JSONL 对应帧 0/3/6/9/12、请求 1～5，全部成功。
- 本轮启动的服务端在主动停止前仍运行，收到 SIGTERM 后在宽限期内正常退出并被回收。
- 服务端退出以后，回放仍能完成，生成程序返回 0。
- 新回放实际可解码出 13 帧、640×360、10 FPS，六张预览名称与尺寸正确且可读取。
- 汇总报告成功写入新的输出目录。

这验证的是多个组件共同工作的**一条正常业务路径**。“端到端”说明跨越哪些组件，不等于覆盖所有异常、并发、算法精度和性能。我们先建立这一条可重复的基线，性能观察后续再讲。

## 2. 什么是 subprocess：Python 可以成为父进程

subprocess 是 Python 标准库里的进程管理模块。你在 shell 输入一个命令时，shell 启动目标程序；现在由 Python 来做启动、等待和收集返回状态这些事。

本课没有把 C++ 程序“导入 Python”。C++ 服务端仍是独立进程，有自己的 PID、地址空间和生命周期，Python 保存的是管理这个进程的对象。

先看一个概念片段，**不必单独执行**：

~~~python
process = subprocess.Popen(["/路径/vision_pool_server"])
print(process.pid)
~~~

Popen 构造成功后返回一个进程对象。它说明创建／执行流程没有向父进程报告启动失败，不等于业务已经就绪：服务端还可能在 bind 时发现端口占用，随后退出。

一个长期服务需要与后续客户端同时存在，所以不能一启动就一直 wait。短命令也可以用 subprocess.run 启动并等待；本课统一使用 Popen，让所有子进程都经过可见的等待与清理逻辑。API 的基本约定见 [Python subprocess 官方文档](https://docs.python.org/3.11/library/subprocess.html)。

### 2.1 命令为什么写成列表

~~~python
[sys.executable, "-u", str(ROOT / "python" / "video_client.py"),
 "--input", str(SOURCE), "--output", str(run_dir / "video.jsonl"),
 "--stride", "3", "--max-frames", "5"]
~~~

每个元素对应一个参数。不要手工把含空格的路径拆开，也不要把引号字符加进参数字符串。

- sys.executable 指向当前 Python 解释器，使子客户端沿用你运行集成脚本的 visionflow 环境。
- -u 使 Python 子程序及时输出日志，便于失败时定位进行到了哪一步。
- 默认 shell=False，不额外启动 shell；列表里的 >、|、$ 等不会变成 shell 操作。
- cwd=ROOT 设置子进程工作目录；ROOT 通过脚本自身路径推导，不依赖用户恰巧站在哪个目录。

ROOT 的计算 `Path(__file__).resolve().parents[1]`：脚本在 python/ 目录中，parents[0] 是 python，parents[1] 才是 VisionFlow_learn。命令里的文件参数也使用绝对路径。

### 2.2 poll 与 wait 的区别

| 调用／结果 | 本课含义 |
| --- | --- |
| process.pid | 这次创建的直接子进程 PID |
| process.poll() 返回 None | 此次检查尚未观察到子进程结束 |
| process.poll() 返回整数 | 已观察到结束，并获得返回码 |
| process.wait(timeout=60) | 等待该子进程结束，返回码可用后返回 |
| wait 超过 timeout | 抛出 TimeoutExpired；仅调用 wait 不会自动杀掉孩子 |
| 返回码 0 | 程序报告成功，仍需检查业务产物 |
| 正整数 | 程序正常退出，但报告该退出码 |
| 负数 -N（POSIX） | 直接子进程被编号 N 的信号终止 |

特别注意：Python 直接等待被 SIGKILL 终止的进程，通常得到 -9；你以前看到的 shell／教学监管器返回码 137 是 128+9 的另一层映射。不要混用。

本课不把 poll() 的一次 None 当作“未来也不会退出”；它只是某次观察。脚本在关键阶段继续检查，业务请求也会暴露随后的失败。

## 3. 日志、就绪和子进程隔离

### 3.1 将子进程输出交给独立日志文件

start_process 给子进程设置：

~~~python
stdin=subprocess.DEVNULL
stdout=log
stderr=subprocess.STDOUT
start_new_session=True
~~~

- stdin 指向空输入，脚本中的子程序不抢你当前终端的输入。
- stdout 写入该步骤的日志文件。
- stderr 与 stdout 写入同一个文件，错误和正常消息都能保留。
- start_new_session=True 在本课 Linux/WSL 环境中让子进程进入新会话，脱离父脚本所在的终端前台进程组。你按 Ctrl+C 时，由父脚本统一进入清理流程。

本课是 Linux/WSL 教学，不宣称这些信号操作在 Windows 原生环境中行为完全一样。

这里不用 stdout=PIPE 后放着不读。管道容量有限，孩子若持续输出而父进程只 wait，可能因管道塞满而互相等待。写文件避免这一种死锁来源，但文件写入依然可能失败或受磁盘影响。

日志以 xb 模式新建，父进程保持文件打开直到孩子已结束。子进程退出后文件才关闭；不会丢弃失败时的目录来掩盖诊断信息。

### 3.2 不用“睡一秒”假装就绪

新服务端的启动速度随机器变化。脚本最多等待 10 秒，每 50 毫秒检查：

1. 本次服务端是否已经退出；退出立即失败。
2. 本轮 server.log 是否同时出现本次 PID 和既有“正在监听”消息。
3. 满足时再开始视频请求；到期仍不满足则失败并清理。

现有 C++ 源码在 bind/listen、epoll 和工作线程建立完成后用 std::endl 输出这些消息。因此本课可利用**本次子进程的独立日志**作为启动阶段观察点。它是与当前日志格式绑定的教学约定，不是通用健康检查协议；不要任意修改 READY_TEXT 或对应服务端提示。

只探测 19090 端口可能误连你之前启动的旧服务端。这里必须先看到本轮子进程自己的监听记录；如果旧服务端占用端口，新孩子会绑定失败，整轮失败，不借旧服务冒充成功。脚本不会杀占用端口的其他进程。

等待截止时间使用 time.monotonic，它适合计算经过的时间，不受系统时钟校准影响；50 毫秒只控制轮询频率，不证明服务端能在 50 毫秒内启动。[Python time 文档](https://docs.python.org/3.11/library/time.html#time.monotonic)

## 4. 清理：成功和失败都要处理自己启动的孩子

stop_process 的顺序是：

~~~text
poll
 ├─ 已退出 → 返回已收集的状态
 └─ 仍运行 → SIGTERM → 最多等待 5 秒
                         ├─ 已退出 → 返回状态
                         └─ 超时 → SIGKILL → 再等待最多 5 秒
                                             ├─ 已退出 → 返回状态，forced=True
                                             └─ 仍未确认 → 报错并保留 PID
~~~

发送信号不是回收。kill 后仍需 wait；如果连强杀后的等待也超时，不能输出“清理成功”。

5 秒是本课验证脚本选择的等待参数，跟原 C++ 监管器 3000 毫秒以及服务端连接 30 秒期限不同。本轮先等五帧客户端结束，再停止服务端，预期没有待处理业务；若必须强杀才能停止，即使输出文件已生成，本轮也判失败。

run_step 在 finally 中调用 stop_process，因而正常完成、非零返回码、等待超时、第一次 Ctrl+C 都会走已登记子进程的收尾。已退出时清理函数直接返回；同一对象再次清理也不会重新发送停止信号。

清理期间暂时将父脚本 SIGINT 处理设为 SIG_IGN，随后恢复，避免连续 Ctrl+C 打断 wait。这个设置只在父脚本主线程中使用；本课没有其他 Python 线程。[Python signal 文档](https://docs.python.org/3.11/library/signal.html)

边界要说清：

- 本课直接启动服务端、客户端和回放程序，没有套 process_supervisor；这些入口不会再创建子进程，所以只管理本次直接孩子。C++ 工作线程属于服务端进程，不是孙进程。
- start_new_session 不等于管理整棵进程树。以后若某个入口再 fork，当前按直接 PID 停止的逻辑就不足。
- SIGKILL、解释器崩溃、机器断电不保证执行 finally；进程创建交接也不是不可打断的事务。
- timeout 是这些等待调用的限度，不是整份脚本的硬实时结束保证。创建进程、文件 I/O、父脚本内 OpenCV 解码等没有由此获得硬期限。
- 若清理本身失败，优先报告清理问题并保留 PID；不要据先前某个子命令成功就宣称整轮成功。

## 5. 检查什么：返回码与产物都需要

程序返回 0 只能证明它按自己的规则报告成功。集成脚本再把这些结果与本课固定素材的预期比较：

| 检查对象 | 预期 |
| --- | --- |
| JSONL | 恰好五条，帧 0/3/6/9/12，请求 1～5，全部 ok |
| 源视频、尺寸、通道 | 原样保留的 lesson11_demo.avi，640×360，3 通道 |
| 时间字段 | 标称 FPS 10，估计时刻 0/300/600/900/1200 毫秒 |
| 算法响应 | processor=lane_lines，边缘计数为合理范围内的整数 |
| 回放视频 | 实际顺序解码 13 帧，每帧 640×360×3，报告 FPS 约 10 |
| 预览 | 帧 0/1/3/6/9/12，共六张，名称正确且都可读取 |

这里复用 11-2 的 load_records 进行基础结构校验，再检查本轮独立指定的预期值。导入 video_replay 不会执行其 main，因为文件末尾有 `if __name__ == "__main__"` 保护。但共用同一校验函数也有共同遗漏的可能，不能宣称独立证明整个解析器正确。

不把重新编码后的边缘数锁死为 2095，不要求 Hough 永远输出某组三条坐标。当前脚本也不检查绿色线段是否画到正确位置；PNG 可读、尺寸正确与绘图位置正确是不同结论，不以此补填 11-2 缺失的可视观察。

本课用 require(condition, message) 显式抛错，不用 Python assert 承担必要检查，因为 assert 可以被优化模式移除。错误即停止本轮，输出明确原因；未到全部检查结束，不生成成功报告。

## 6. 输出目录和前置条件

每轮用一个不存在的新目录，例如：

~~~text
outputs/e2e_01/
    server.log
    video_client.log
    video_replay.log
    video.jsonl
    replay/
        replay.avi
        frame_000000.png
        frame_000001.png
        frame_000003.png
        frame_000006.png
        frame_000009.png
        frame_000012.png
    report.json
~~~

失败时部分文件可能已存在，它们保留供排查。下一轮换 e2e_02，不自动删除、清空或复用旧目录。report.json 是一份整体报告，因此可以 indent=2；video.jsonl 仍是一行一条记录，两者不同。

前置条件都来自已完成的课程：

- 已有 build/vision_pool_server 可执行文件。
- 已有 python/video_client.py 和 python/video_replay.py。
- 已有 samples/lesson11_demo.avi：你此前的 20 帧、640×360、10 FPS 素材，保持原样。
- 在 visionflow 环境运行，能够导入 cv2。
- 19090 没有被你之前的服务端占用。若还有旧实例，由你回到它的终端正常停止。

脚本不会代替你安装、配置或构建，不包含编译命令，也不启动旧监管器。本课命令由你运行时，才会自动执行下面定义的课程验证。

## 7. 新建主要文件，写入完整代码

新建 `/home/rikanshin/VisionFlow/VisionFlow_learn/python/integration_check.py`。

建议按下面四块依次阅读和输入，但保留为同一个完整知识点：

1. 路径、参数、require 与 start_process。
2. stop_process、wait_ready、run_step 三个生命周期函数。
3. verify_outputs 的业务预期。
4. main 串联和最外层错误处理。

不用一边写一边启动服务；全部写完、理解流程后，再执行第 9 节的一次完整实践。

~~~python
import argparse
import json
import math
from pathlib import Path
import signal
import subprocess
import sys
import time

import cv2

from video_replay import load_records


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "samples" / "lesson11_demo.avi"
SERVER = ROOT / "build" / "vision_pool_server"
EXPECTED_FRAMES = [0, 3, 6, 9, 12]
READY_TEXT = "线程池服务端正在监听 127.0.0.1:19090"


class LessonParser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, f"参数错误：{message}\n请使用 --help 查看用法。\n")


def parse_args():
    parser = LessonParser(
        description=(
            "用法：python3 python/integration_check.py --output-dir 新目录\n"
            "启动已有服务端，验证五帧请求、离线回放与正常停止。"
        ),
        usage=argparse.SUPPRESS,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        add_help=False,
    )
    options = parser.add_argument_group("参数")
    options.add_argument("-h", "--help", action="help", help="显示帮助后退出")
    options.add_argument("--output-dir", required=True, type=Path,
                         help="保存本轮日志和结果的新目录")
    return parser.parse_args()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def start_process(command, log):
    return subprocess.Popen(
        command,
        cwd=ROOT,
        stdin=subprocess.DEVNULL,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


def stop_process(process, label):
    # 清理过程中忽略后续 Ctrl+C，避免第二次中断打断 wait。
    old_handler = signal.signal(signal.SIGINT, signal.SIG_IGN)
    forced = False
    try:
        code = process.poll()
        if code is not None:
            return code, forced
        print(f"停止{label}：PID={process.pid}，先发送 SIGTERM", flush=True)
        try:
            process.terminate()
        except ProcessLookupError:
            pass
        try:
            code = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            forced = True
            print(f"{label}未按期退出，发送 SIGKILL", flush=True)
            try:
                process.kill()
            except ProcessLookupError:
                pass
            try:
                code = process.wait(timeout=5)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(
                    f"{label}已尝试强杀但仍未确认回收，PID={process.pid}"
                ) from error
        print(f"{label}已回收，返回码={code}", flush=True)
        return code, forced
    finally:
        signal.signal(signal.SIGINT, old_handler)


def wait_ready(process, path):
    deadline = time.monotonic() + 10
    while True:
        code = process.poll()
        require(code is None, f"服务端在就绪前退出，返回码={code}，查看 {path}")
        with path.open("rb") as stream:
            text = stream.read(64 * 1024).decode("utf-8", errors="replace")
        own_pid = f"服务端 PID：{process.pid}\n"
        if own_pid in text and READY_TEXT in text:
            require(process.poll() is None, "服务端打印监听信息后已退出")
            print(f"本次服务端已报告监听：PID={process.pid}", flush=True)
            return
        require(time.monotonic() < deadline, f"等待监听超过 10 秒，查看 {path}")
        time.sleep(0.05)


def run_step(label, command, log_path):
    print(f"开始{label}；日志：{log_path}", flush=True)
    with log_path.open("xb") as log:
        process = start_process(command, log)
        try:
            try:
                code = process.wait(timeout=60)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(f"{label}超过 60 秒，查看 {log_path}") from error
            require(code == 0, f"{label}返回码={code}，查看 {log_path}")
            print(f"{label}完成，返回码=0", flush=True)
            return code
        finally:
            stop_process(process, label)


def verify_outputs(run_dir):
    records, source, size = load_records(run_dir / "video.jsonl")
    require(source == SOURCE.resolve(), "记录指向了错误的源视频")
    require(size == (640, 360), "记录尺寸不是本课的 640×360")
    require([item["frame_index"] for item in records] == EXPECTED_FRAMES,
            "记录的帧下标不是 0、3、6、9、12")
    require([item["request_id"] for item in records] == [1, 2, 3, 4, 5],
            "请求编号不是 1～5")
    for item in records:
        require(item["status"] == "ok", "本次顺序请求存在失败记录")
        result = item["result"]
        require(result.get("processor") == "lane_lines", "处理器不符合预期")
        require(type(result.get("channels")) is int and result["channels"] == 3,
                "响应通道数不符合预期")
        require(type(result.get("edge_pixels")) is int and
                0 <= result["edge_pixels"] <= 640 * 360, "边缘计数不合理")
        fps = item.get("source_fps")
        require(type(fps) in (int, float) and math.isfinite(fps) and
                abs(fps - 10.0) <= 0.01, "记录标称 FPS 不符合本课素材")
        stamp = item.get("estimated_video_time_ms")
        require(type(stamp) in (int, float) and math.isfinite(stamp) and
                abs(stamp - item["frame_index"] * 100.0) <= 0.01,
                "估计视频时刻与帧下标不对应")
    print("JSONL 检查通过：五条成功记录，帧和请求编号对应", flush=True)

    replay_dir = run_dir / "replay"
    capture = cv2.VideoCapture(str(replay_dir / "replay.avi"))
    try:
        require(capture.isOpened(), "无法打开本轮回放视频")
        fps = float(capture.get(cv2.CAP_PROP_FPS))
        require(math.isfinite(fps) and abs(fps - 10.0) <= 0.01,
                "回放 FPS 不是 10")
        count = 0
        while True:
            readable, frame = capture.read()
            if not readable:
                break
            require(frame is not None and frame.shape == (360, 640, 3),
                    "回放出现空帧或尺寸、通道错误")
            count += 1
            require(count <= 13, "回放视频比预期多出帧")
        require(count == 13, f"回放实际只有 {count} 帧，预期 13")
    finally:
        capture.release()
    print("回放解码通过：13 帧，640×360，10 FPS", flush=True)

    expected_names = {
        f"frame_{index:06d}.png" for index in [0, 1, 3, 6, 9, 12]
    }
    actual_names = {path.name for path in replay_dir.glob("*.png")}
    require(actual_names == expected_names, "预览图片名称或数量不符合预期")
    for name in sorted(expected_names):
        preview = cv2.imread(str(replay_dir / name))
        require(preview is not None and preview.shape == (360, 640, 3),
                f"预览图片无法读取或尺寸不正确：{name}")
    print("预览文件检查通过：六张 PNG 可读取，尺寸正确", flush=True)
    return {"records": len(records), "decoded_frames": count,
            "width": 640, "height": 360, "fps": fps, "previews": 6}


def main():
    args = parse_args()
    run_dir = args.output_dir.resolve()
    for path in [SERVER, SOURCE, ROOT / "python" / "video_client.py",
                 ROOT / "python" / "video_replay.py"]:
        require(path.is_file(), f"缺少前置文件：{path}")
    require(not run_dir.exists(), f"输出目录已存在，请换新目录：{run_dir}")
    run_dir.mkdir(parents=True, exist_ok=False)
    print(f"本轮输出目录：{run_dir}", flush=True)

    server_log = run_dir / "server.log"
    with server_log.open("xb") as log:
        server = start_process([str(SERVER)], log)
        try:
            wait_ready(server, server_log)
            video_code = run_step(
                "视频请求",
                [sys.executable, "-u", str(ROOT / "python" / "video_client.py"),
                 "--input", str(SOURCE), "--output", str(run_dir / "video.jsonl"),
                 "--stride", "3", "--max-frames", "5"],
                run_dir / "video_client.log",
            )
            require(server.poll() is None, "服务端在主动停止前已退出")
            server_code, forced = stop_process(server, "服务端")
            require(not forced and server_code == 0,
                    "服务端没有在宽限期内正常退出，本轮不通过")
        finally:
            stop_process(server, "服务端")

    # 此时本次服务端已退出且被回收，下面才开始离线回放。
    replay_code = run_step(
        "离线回放",
        [sys.executable, "-u", str(ROOT / "python" / "video_replay.py"),
         "--records", str(run_dir / "video.jsonl"),
         "--output-dir", str(run_dir / "replay")],
        run_dir / "video_replay.log",
    )
    facts = verify_outputs(run_dir)
    report = {
        "status": "ok",
        "scope": "lesson11_sequential_e2e",
        "source": str(SOURCE.resolve()),
        "server_pid": server.pid,
        "returncodes": {"server": server_code, "video_client": video_code,
                        "video_replay": replay_code},
        "artifacts": facts,
        "limits": ["不验证绘图位置和算法精度", "不验证并发与过载",
                   "不验证忙碌时停止", "不测量性能"],
    }
    with (run_dir / "report.json").open("x", encoding="utf-8") as output:
        json.dump(report, output, ensure_ascii=False, indent=2, allow_nan=False)
        output.write("\n")
    print(f"本轮端到端集成验证通过；报告：{run_dir / 'report.json'}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("用户中断集成验证；已进入已登记子进程的清理流程", file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, cv2.error) as error:
        print(f"集成验证失败：{error}\n保留本轮目录供排查。", file=sys.stderr)
        sys.exit(1)

~~~

## 8. 按调用关系读一遍

### 8.1 start_process：只负责启动，返回“管理句柄”

command 是参数列表，log 是已经以二进制方式打开的日志文件对象。Popen 接管子进程的标准输出描述符，函数返回 process。父脚本没有用全局变量保存“最近一个 PID”，每个函数管理传进来的确切对象。

创建失败会抛 OSError，例如文件不存在、没有执行权限。创建成功后目标程序自行报错，则通常表现为稍后 poll/wait 取得非零返回码；这两条失败路径不同。

### 8.2 wait_ready：长服务先活着，再提供启动阶段证据

deadline 只算一次，不在每轮循环后重新加 10 秒。每轮先检查 poll，再读本轮日志前 64 KiB。启动消息就在开头；本课受控日志只需读取有限前缀，不需要不断积累整个服务日志。

own_pid 中包含完整的换行，匹配本次孩子的 PID 行；READY_TEXT 匹配现有源码的监听消息。双重条件成立后还检查一次 poll。这个观察点之后的真实五帧成功响应才提供业务路径证据，日志文字本身不证明完整业务健康。

### 8.3 run_step：启动一个短步骤，等待并确认它正常完成

log_path.open("xb") 创建该步骤日志。run_step 调用 wait(timeout=60)，返回 0 时打印成功并返回这个数；非零返回码通过 require 变成 ValueError。

等待超时转换为带步骤名称和日志路径的 RuntimeError，随后 finally 仍执行。这里没有写“超时即自行消失”：先 TERM、必要时 KILL、最后 wait 的清理由 stop_process 承担。

即使 try 中写了 return，Python 也会先执行 finally，再把值返回给调用者。若 finally 抛异常，则不会沿原来的 return 成功返回。这是把清理是否成功纳入整轮结果的一部分。

### 8.4 stop_process：返回两个信息，不把强杀算作正常停止

返回二元组 (code, forced)：

- code 表示实际收集的进程返回状态。
- forced=True 表示本次停止流程已经升级到 SIGKILL。
- 孩子原本已退出时，直接得到 code，forced=False；这并不表示它一定正常退出，还要看 code。

main 在正常停止点先要求 server.poll() 仍为 None，然后要求 not forced 且 code==0。非零或需要升级强杀时，本轮失败。finally 中再调用一次用于异常兜底；已回收对象不再发信号。

ProcessLookupError 表示发信号时目标已不存在，可能恰好在 poll 和 terminate/kill 之间退出；这时继续 wait 获取状态，而不是凭发送失败猜测返回码。其他 OSError 没有被一概吞掉。

### 8.5 verify_outputs：验证本轮产物，返回检查事实

先从本轮 video.jsonl 载入记录。load_records 的基础检查通过以后，还要比较恰好五个预期 frame_index 和 request_id；不是“任意五条合法数据都算成功”。

回放检查使用逐帧 read 计数，读到第 14 帧立即失败，提前结束且少于 13 帧也失败。这个做法检验受控产物的长度，不宣称实现任意视频的损坏鉴定。

六张 PNG 使用文件名集合精确比较，然后逐张解码看 shape；只检查 existence 会把空文件误算作有效图片。返回字典记录已经满足的产物条件，供成功报告使用。

### 8.6 main：服务停止必须早于离线回放

main 里 server 的 try/finally 包住“就绪、请求、主动停止”。离开这部分后，才调用回放。因此本次回放不依赖该服务端继续存在。

报告中的三个 returncodes 分别来自本次服务端、视频客户端、回放客户端，不复制 shell 某次 echo $?，也不把三个数合成一个就丢掉细节。

报告只在这条正常业务路径和必要清理都通过后写入。失败时根据控制台定位具体步骤，再看对应日志。若写报告本身失败，脚本同样返回 1；看见一个 report.json 文件名也不能替代检查它完整可解析和整轮退出码。

## 9. 由你执行一次集成实践

### 9.1 先确认入口

完成主要文件后，在 visionflow 环境的学习工程根目录执行。无需另开服务端终端；本次服务端由脚本启动。若你之前手动启动的服务端仍在运行，先回到那个终端正常停止。

不要重建原有视频、改写旧 JSONL，也不要把输出目录指定为 outputs/replay_01。

### 9.2 执行主命令

~~~bash
python3 python/integration_check.py --output-dir outputs/e2e_01
echo $?
~~~

若 e2e_01 已存在，改用 e2e_02，后续查看路径一并调整。

预期核心输出如下，PID 和绝对路径因运行而异：

~~~text
本轮输出目录：.../outputs/e2e_01
本次服务端已报告监听：PID=...
开始视频请求；日志：.../video_client.log
视频请求完成，返回码=0
停止服务端：PID=...，先发送 SIGTERM
服务端已回收，返回码=0
开始离线回放；日志：.../video_replay.log
离线回放完成，返回码=0
JSONL 检查通过：五条成功记录，帧和请求编号对应
回放解码通过：13 帧，640×360，10 FPS
预览文件检查通过：六张 PNG 可读取，尺寸正确
本轮端到端集成验证通过；报告：.../report.json
~~~

shell 退出码预期 0。以上是示例预期，老师没有代跑。

### 9.3 查看本轮报告与服务端收尾日志

~~~bash
cat outputs/e2e_01/report.json
tail -n 12 outputs/e2e_01/server.log
~~~

报告中应有：

~~~json
{
  "status": "ok",
  "scope": "lesson11_sequential_e2e",
  "returncodes": {
    "server": 0,
    "video_client": 0,
    "video_replay": 0
  },
  "artifacts": {
    "records": 5,
    "decoded_frames": 13,
    "width": 640,
    "height": 360,
    "fps": 10.0,
    "previews": 6
  }
}
~~~

这是摘录，实际报告还含 source、server_pid 和 limits。服务端日志结尾应有收到 SIGTERM、停止入口、任务与连接处理完毕、工作线程回收和正常退出等消息。不要要求连接 token 与 request_id 相同，也不要求某个 worker 固定负责哪一帧。

请反馈主命令输出、紧接着的退出码、报告与服务端日志结尾。失败时贴控制台原因和它指向的日志即可；保留目录，先分析，不反复重跑。

本轮不要求故意占用端口、删除素材、制造损坏记录或重复 Ctrl+C。示例有相应处理逻辑，不等于这些分支已实际验证。更不要求为旧课补跑错误分支。

## 10. 如何定位失败

| 首个失败位置 | 先检查 |
| --- | --- |
| import cv2 / video_replay 失败 | 当前解释器环境、文件名和脚本位置 |
| 缺少前置文件 | 现有 build/vision_pool_server、素材和两个 Python 入口 |
| 创建进程失败 | 路径、执行权限、运行环境 |
| 服务端就绪前退出 | server.log；常见原因是旧服务占用 19090 |
| 等待监听超时 | server.log 是否出现当前 PID 与原有监听文字；是否卡在初始化 |
| 视频请求非零退出 | video_client.log 与 server.log，对照本轮 video.jsonl |
| 视频请求少于五条却返回 0 | verify_outputs 会报帧下标不匹配；检查素材长度或解码失败 |
| 服务端停止需要强杀 | server.log；不能用后续产物存在来抵消停止失败 |
| 回放非零退出 | video_replay.log；记录格式、源路径、尺寸、写入后端 |
| PNG 名称／数量不符 | 回放保存逻辑、本次是否使用了正确新目录 |
| 回放帧数不足 | video_replay.log、素材和实际编码文件，不只看写入计数 |
| 已尝试强杀仍未确认回收 | 保留控制台 PID，不宣称无残留；先分析进程状态 |

注意 shell 中的 echo $? 必须紧接要检查的命令。你如果先 cat 或 tail，再 echo，得到的是最后那个查看命令的退出码。

## 11. 集中理解检查

主要实践之后，一次回答：

1. 为什么启动服务端用 Popen 后不能马上 wait 到它退出？Popen 创建成功、poll 返回 None、日志报告监听、五帧业务成功分别说明什么？
2. 命令列表、sys.executable、cwd、日志重定向分别解决什么问题？为什么 stdout=PIPE 后只 wait 可能卡住？
3. Popen.wait 超时会自动停止子进程吗？finally 在 return 或异常时是否执行？terminate、kill、wait 分别承担什么职责？
4. 为什么本次服务端要在回放之前停止？为什么返回码全部为 0 后，还要读回 JSONL、AVI 和 PNG？六张 PNG 能读是否证明线段位置正确？
5. Python 直接等到被 SIGKILL 终止的孩子，返回码 -9 与以前的 137 有什么区别？本课成功是否证明并发过载、忙碌时停止、算法精度或性能？5 秒、10 秒、60 秒是否构成整轮硬实时保证？

## 11.1 五点直接解答（2026-10-14）

学生要求“帮我回答”，老师直接解释如下，不记作学生独立答对，不要求重答原题。当时只解答当前知识点；学生随后要求“进行下一步”，最新接续状态见第 12 节。

1. **启动、存活、监听与业务成功是四层证据。** 服务端是长期运行的程序，Popen 后立即无期限 wait 会让父脚本停在“等服务端退出”，无法继续启动客户端；本课应保留服务端进程对象，先等就绪，再执行客户端。Popen 返回表示创建／执行阶段没有向父进程报告启动失败，不代表 bind/listen 成功；poll 返回 None 表示这次尚未观察到退出，不证明程序没有卡住或未来不会退出；本轮独立日志出现自身 PID 和监听消息，说明当前源码已执行到监听及相关初始化后的输出点；五帧请求获得并通过校验的响应，才是本轮业务路径能够完成的证据。业务成功只覆盖本次固定素材和请求，不覆盖所有情况。
2. **参数列表明确参数边界，解释器与 cwd 明确环境和路径，日志留下各阶段证据。** 命令列表的每个元素就是一个参数，含空格的路径仍是一项；本课 shell=False，不把字符串中的重定向符号当成 shell 指令。sys.executable 选择当前 Python 解释器，使子脚本使用当前 visionflow 环境的解释器；cwd=ROOT 设置子程序工作目录，避免相对路径随启动位置变化；stdout=log、stderr=STDOUT 将该步骤两类输出写到独立文件，失败后可以追溯。如果设 stdout=PIPE 却只 wait、不读管道，孩子可能写满有限容量的管道并阻塞，父进程又在等孩子退出，形成互相等待。若采用 PIPE，应正确读取，例如适用时使用 communicate；本课直接写日志文件。
3. **wait 超时只报告等待失败，停止与回收由清理代码承担。** Popen.wait(timeout=60) 超时抛 TimeoutExpired，不自动杀掉子进程。正常的 return 和异常传播都会先经过 finally；但父进程被 SIGKILL、机器断电等不能保证执行 Python 清理。本课 Linux/WSL 下，terminate 向直接孩子发 SIGTERM，让程序有机会自行清理退出；kill 发 SIGKILL 强制终止，目标无法执行正常清理；wait 等待结束并取得退出状态、完成回收。发送信号并不等于已回收，所以强杀后仍需 wait。清理失败会阻止原先的成功 return，不能忽略。注意这说的是 Popen.wait，不要与 subprocess.run 的 timeout 自动杀掉并等待直接子进程的行为混淆。
4. **先停止服务端验证回放不依赖它，读取产物验证程序的实际输出。** 本轮确认服务端已结束并回收后才启动回放，成功结果说明这条回放路径只依赖现有记录与视频，不要求该服务端继续运行。子程序返回 0 是程序对自身执行结果的报告，仍可能有实现遗漏，例如只产生少数记录或写出无法读回的文件。因此脚本再检查具体帧／请求对应、五条成功记录、13 帧 640×360/10 FPS 视频和六张 PNG。PNG 名称、尺寸正确且可读取，并不证明绿色线段位置正确、标注文字正确或算法检测准确；本课没有验证这些内容，也不能据此补填 11-2 缺失的可视观察。
5. **-9 与 137 是不同层对信号终止的表示，局部超时不组成全局硬期限。** Python subprocess 在 POSIX 上用负数 -N 表示直接孩子被信号 N 终止，SIGKILL 为 9，所以是 -9。之前 shell／教学监管器使用 128+9=137 的映射；单独看到正常退出码 137 并不能证明该进程本身被 SIGKILL 杀死，它也可能主动退出 137，要看所在层的状态信息。本轮仅覆盖顺序正常路径，不能证明并发过载、忙碌时停止、算法精度或性能；本次 SIGTERM 到达时任务数和连接数都为 0。10 秒用于等待监听，60 秒用于等待某个短子步骤，5 秒分别用于 TERM 后、必要时 KILL 后的等待；创建进程、文件 I/O、父脚本里的 OpenCV 解码等不被这些数字整体包住，因此不能相加后宣称整轮一定在某个时限内结束。

## 12. 当前状态与后续边界

2026-10-14（11-3 按学生要求收尾）：学生在老师直接解释五点后明确要求“进行下一步”。结合此前源码核对和 outputs/e2e_01 的主要集成实践，按要求收尾 11-3，进入性能观察；不记作学生独立答对，也不重问原题或要求补跑。未提供的可视、忙碌停止及异常分支证据仍不倒填。

当前进入 [11-4 性能观察](11-4-performance-observation.md)，熟悉程度已确认“完全不熟悉，需要从头讲”。以下保留此前各阶段记录，以本段最新状态为准。

2026-10-14（11-3 五点由老师直接解答，理解确认待完成）：学生要求“帮我回答”。老师已解释启动／存活／监听／业务成功的证据层次，命令列表与解释器／cwd／日志重定向，未读取 PIPE 的互相等待，Popen.wait 超时不自动停止、finally 的执行与边界、TERM/KILL/wait 分工，停止服务端后离线回放及返回码之外的产物检查，以及 -9/137 的层次区别、验证范围和局部超时的边界。完整解答见讲义第 11.1 节。不记作学生独立答对，不重复原题或要求重跑；接续学生的疑问或理解确认。本次没有要求进入下一环节，不自动收尾 11-3 或生成性能观察课程。此前主要实现与运行实践仍已通过，未提供的可视和异常分支证据不倒填。老师仅维护教学资料，没有改写源码或运行、测试。

2026-10-14（11-3 实现与主要集成实践通过，集中理解检查待完成）：学生已创建 python/integration_check.py，老师只读核对与讲义完整示例一致（忽略换行格式及末尾空白）。学生运行 --output-dir outputs/e2e_01，启动本轮服务端 PID 4913；五帧视频请求完成且返回码 0，随后发送 SIGTERM，服务端正常退出并被回收、返回码 0，没有升级强杀；之后才执行离线回放，返回码 0。主程序检查五条成功记录和帧／请求对应、实际回放 13 帧、640×360、10 FPS，以及六张名称尺寸正确且可读取的 PNG，并输出验证通过。报告 status=ok、scope=lesson11_sequential_e2e，三个子进程 returncodes 均 0，产物计数与控制台一致。老师只读核对 report.json、server.log、video_client.log、video_replay.log；服务端五次请求依次处理，收到 SIGTERM 时未交回任务 0、当前连接数 0，打印停止入口、工作线程回收和正常退出。学生未展示紧接主命令的 echo $?，不倒填父脚本的 shell 退出码，也不要求因此重跑；已有成功报告和日志足以确认本课主要正常路径。PNG 文件可读不等于绘图位置正确，不补填 11-2 未提供的可视观察；不据五条耗时推断性能，也不宣称并发过载、忙碌停止、超时强杀或其他异常分支已经验证。当前只接续讲义第 11 节五道集中理解检查，不重复熟悉程度、不提前进入性能观察。老师仅维护教学资料，未修改源码或产物、配置、构建、运行或测试。

以下保留此前准备记录，以本段最新状态为准。

2026-10-13：学生要求“帮我回答，然后进入下一环节”。11-2 五点已由老师直接解释，按明确要求收尾，不记作独立答对；两张 PNG 可视观察仍未收到反馈，不倒填，也不要求补跑。

11-3 开课时熟悉程度为“完全不熟悉，需要从头讲”，不再询问。python/integration_check.py 已由学生亲手创建，实现与主要集成运行观察通过；五点由老师按要求直接解答后，学生要求进入下一步，据此收尾并接续 11-4。老师仅维护教学资料，没有改写主要源码、配置、构建、运行或测试。

本课范围为固定素材上的顺序端到端功能验证，直接管理自建子进程并保留每轮新目录。它不是生产进程监管器，不保证应对父进程强杀，不管理任意后代树，不验证算法精度、并发过载、忙碌时停止，也不提供性能结论。后续再安排性能观察，不提前实现。

源码示例已按现有文件接口静态核对，外部 API 对照官方文档；本课主要正常路径已有上述学生运行证据，其余分支不记作已验证。
