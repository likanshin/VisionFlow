# 11-4：固定图片的并发请求与性能观察

熟悉程度已确认：“完全不熟悉，需要从头讲”。11-3 已完成主要实践，五点由老师直接解释后，你要求“进行下一步”，据此收尾并进入本课，不记录为你独立答对原题。

本课只增加一个由你亲手编写的入口：
`/home/rikanshin/VisionFlow/VisionFlow_learn/python/performance_observe.py`。

老师维护讲义、README 和 HANDOFF；不会代你创建主要源码、配置、构建、运行或测试。本课复用原协议和服务端，不修改 CMake、算法或队列参数。

## 1. 从两个问题开始：等多久，完成多少

功能验证关心“是否得到正确格式的结果”。性能观察还要问：

- 发出一个请求以后，需要等多久才能得到响应？
- 同一段时间里，一共完成了多少成功请求？

第一个是**请求延迟**，单位通常是毫秒；第二个是**成功吞吐量**，单位是请求／秒。它们有关联，但不是同一个量。

举例：两名顾客各等 100 毫秒，同时得到结果。对每个人，延迟都是 100 毫秒；从整批角度，约 0.1 秒完成两件事，成功吞吐约 20 请求／秒，而不是 10。

这种简化例子忽略启动、收尾与调度开销。真实数字要按实际计时算，不能只用“两个工作线程”推导必然翻倍。

### 1.1 并发数是什么意思

本课的并发数 C 是客户端同时执行请求的数量上限：

~~~text
C=1：请求 A ─等待响应─ 请求 B ─等待响应─ 请求 C
C=2：线程 1：请求 A ─等待响应─ 请求 C ─等待响应─ …
     线程 2：请求 B ─等待响应─ 请求 D ─等待响应─ …
~~~

某个线程收到响应以后，才领取下一项；一项请求仍然新建一个 TCP 连接，符合现有服务端“一连接一请求”的约定。

C=6 不表示服务端有六条图像处理线程。当前服务端仍是两条计算线程；其余请求可能排队或被 busy 拒绝。启动、结束和调度时也不保证每个瞬间恰好有 C 项在途。

本课正式请求总数默认 N=60，C 分别观察 1、2、6。N 是总量，C 是同时在途的上限，两者不能混淆。

## 2. 先定义计时边界，再看数字

本课使用已有 samples/frame.jpg，读取同一份 JPEG 字节并复用。程序在正式计时前解码一次，获得宽高；不会为每次请求再读磁盘或重新编码 JPEG。

### 2.1 单项 latency_ms 测什么

observe_one 在调用 request_image 之前取开始时间，返回或捕获预期异常后取结束时间：

~~~text
[开始计时]
  构造协议消息 → 新建 TCP 连接 → 发送
  → 服务端收包／等待／处理 → 接收响应
  → JSON 和编号、尺寸等校验 → 关闭连接
  → 记录响应类别
[结束计时]
~~~

所以它是当前客户端观察的请求耗时，包含服务端等待和计算，也包含客户端与网络开销。它不是 Canny/Hough 的纯算法耗时。

不包含输入文件读取、那次预先解码、正式测量前预热、领取编号前的锁等待、最终样本落盘。与 11-1 的 elapsed_ms 相比，本课不再包含逐帧 JPEG 编码；不要直接把两课的数字当作同一指标。

time.perf_counter 适合测量经过时间，使用两次读数之差，包含期间等待；不使用日期时间相减来量每个请求。[Python time 官方文档](https://docs.python.org/3.11/library/time.html#time.perf_counter)

### 2.2 整批 batch_elapsed_s 测什么

从创建客户端线程池之前开始，到所有请求循环结束、收集记录并回收线程池之后结束。它包含线程池启动、调度、收集和收尾，排除输入准备、预热、报告统计和写盘。

同一批里多个请求时间会重叠，所以：

**不能把各请求 latency_ms 相加，再把这个和当作整批经过的时间。**

报告中的成功吞吐计算为：

~~~text
success_rps = 正式成功请求数 / 整批实际经过秒数
~~~

这是一批有限请求的平均成功吞吐，不是服务端的峰值能力或长期稳定容量。

## 3. 四种结果必须分开

| 本课状态 | 含义 |
| --- | --- |
| ok | 得到通过现有协议、编号和尺寸等校验的成功响应 |
| busy | 得到合法的服务端过载拒绝，code=busy |
| server_error | 得到其他合法业务错误响应 |
| client_error | 连接、收发、解析或校验发生预期异常 |

本课 ok 不附带新的算法精度证明。复用的 request_image 并不验证“检测线是否真的贴合道路”。

### 3.1 为什么 busy 不能混入成功延迟

假设某批完成 100 项尝试，其中 20 项成功、80 项很快被拒绝。你若把它们混起来算一个平均值，拒绝越多，平均值可能越低；但真正成功的图片并没有因此处理得更快。

因此：

- 延迟按四种状态分别统计。
- success_rps 只数 ok。
- valid_response_rps 数 ok + busy + server_error，表示收到了多少条通过协议检查的响应。
- busy_fraction_observed = busy 数 / 已观测正式请求数。
- 没有收到合法响应的 client_error 不算入有效响应吞吐。

假设上面的 100 项都在 1 秒内结束，则成功吞吐为 20 请求／秒，有效响应吞吐为 100 响应／秒，busy 比例为 80%。只报“100 请求／秒”会掩盖拒绝情况。

如果正式阶段出现 client_error 或其他 server_error，停止领取后续新任务，收齐已经在执行的请求，然后保存不完整／错误报告。这时 busy 比例的分母是**实际记录数**，不是原计划的 60。

busy 不触发停止，它是本课需要观察的过载反馈。每个 busy 都属于一个新的独立尝试，没有自动重试该请求。

### 3.2 退出码的定义

- 0：预热要求满足，正式 N 项全部获得 ok 或 busy，报告保存成功。
- 1：预热失败、正式错误／不完整，或文件等步骤失败。
- 2：命令行参数不符合约定。
- 130：用户中断。

因此返回 0 表示本次测量按规则完成，**不表示 N 项全部成功**。即使正式阶段全部 busy，也可以是完整的测量；成功吞吐会如实为 0，成功延迟为 null。是否可接受要结合业务目标判断。

预热要求每项都 ok，否则本次不开始正式测量。预热失败信息保存在 warmup.jsonl；不能把这种运行当作一份有效性能比较。

## 4. 平均值、P50、P95：看分布，不能只看一个平均数

把同一种状态的延迟从小到大排列：

- 平均值：所有延迟之和除以样本数，容易受少数极慢请求影响。
- P50：处于样本约一半位置的数。
- P95：处于样本约 95% 位置的数，用来观察较慢的那部分请求。
- 最大值：本批最慢的一项，不代表未来的最坏上限。

不同工具的分位数算法可能不同。本课明确采用**最近秩**定义：

~~~text
序号 = ceil(p × n)       # 序号从 1 起算
数组下标 = 序号 - 1
~~~

例如 20 个已排序延迟为 1、2、…、20 毫秒：

- P50 取第 ceil(0.5×20)=10 个，即 10 毫秒。
- P95 取第 ceil(0.95×20)=19 个，即 19 毫秒。
- 平均值为 10.5 毫秒。

所以这里的 P50 不一定等于“偶数样本取中间两项平均”的中位数算法。报告写明 nearest_rank，避免比较时把定义差异当成性能变化。

每组同时记录 n。若仅有 3 项成功，那么该组 P95 就是这 3 项的最大值，不能称为可靠的长期尾延迟；若没有样本，整组统计写 null，不伪造 0 毫秒。

## 5. 预热、负载方式和可比较条件

### 5.1 为什么先预热

刚启动时可能发生首次初始化、代码和数据进入缓存等现象。预热先做默认五次顺序请求，让后续观察少受部分首次开销影响。

预热结果另存 warmup.jsonl，不计入正式 N、正式分位数和整批时间。预热编号 1～5，正式编号从 6 开始；默认正式阶段是 6～65，共 60 个。

五次预热不保证所有缓存和线程都已稳定，也不验证冷启动表现。客户端正式线程池是在预热之后才创建，启动开销仍在整批时间内。

### 5.2 本课是“等响应后再补请求”的闭环

线程收到一个结果才发下一个，因此服务变慢时，客户端发起新请求的速度也随之下降；它不能模拟“无论服务多慢，外部仍固定每秒到来几百个请求”的情况。

busy 返回很快时，空闲客户端线程可能迅速消耗剩余请求编号，而其他成功任务仍在处理。固定 60 项短实验可能主要体现启动、快速拒绝和最后收尾；成功样本可能很少。

所以本课学习如何测量和解释，不用这份短闭环实验宣布生产容量或满足某个延迟承诺。没有自动重试、没有固定到达率，也没有无限压测。

### 5.3 比较时尽量只改变并发数

三组都使用：

- 同一个正在运行的服务端，2 条工作线程、排队容量 2，观察延迟 0。
- 同一张 JPEG、同样的正式请求数和预热次数。
- 相同日志输出方式、机器环境，尽量没有其他客户端同时发请求。

本地客户端和服务端会争用 CPU，Python 调度和响应校验也有开销；OpenCV 内部可能有自己的线程。因此“客户端并发从 1 改成 2”不保证成功吞吐翻倍，改成 6 也不保证更快。

服务端原有逐连接日志仍会影响观测，不在本课改造它；三组保持一致，并承认测到的是当前带日志系统的表现。

配置文件记录输入 JPEG 的 SHA-256 指纹、大小、尺寸、解释器和 OpenCV 版本、并发、请求数和人工填写的服务端说明。指纹用于识别输入字节是否一致；server_note_declared 只是你的声明，客户端没有从服务端读回并验证这些参数。

## 6. Python 怎样保持有限并发

ThreadPoolExecutor 在客户端建立至多 C 条线程。每条线程执行一个循环：领取编号 → 做一次请求 → 保存一条本地记录 → 再领取编号。[Python futures 官方文档](https://docs.python.org/3.11/library/concurrent.futures.html)

这里提交的是 **C 个工作循环**，而不是一次性 submit N 个独立请求。线程池里没有额外堆积 N 项待执行任务；请求来源由 next_id 控制，总数最多 1000，并发最多 8。

next_id 是共享变量，使用 threading.Lock 保护“检查剩余量、取编号、递增”这一整体操作。锁内不做网络请求，因此不会让其他线程为某个请求收发而等待这个锁。

worker 内的 local_rows 只由自己修改，返回后才由主线程汇总；JPEG 是共享的不可变 bytes，各次 request_image 自己创建连接，不共享 socket。

stop 使用 Event 表示停止领取新任务；这和取消已经发送的服务端计算不同。一个线程发现错误时，其他线程可能已经领到编号并进入请求，它们会完成或报错后返回。[Python threading 官方文档](https://docs.python.org/3.11/library/threading.html)

本课 Python 线程主要用于重叠网络等待，不把它当作让 Python 纯计算必然并行加速的方法。

### 6.1 线程收尾与超时边界

主线程用 as_completed 取已结束工作循环的 Future，再用 result 取回该循环的样本列表。它不是每个请求完成就打印一次；正式阶段不逐条打印或落盘，以减少这些操作对测量的干扰。

finally 设置停止标志，再 shutdown(wait=True, cancel_futures=True)。还没启动的工作循环可以取消，已经运行的不能被这句强杀，仍需等待。收尾期间暂时忽略第二次 Ctrl+C，之后恢复处理方式。

复用 request_image 的 socket 超时为 5 秒，但 recv_exact 可能多次 recv；这不构成整次请求或整批实验的统一 5 秒截止时间。[Python socket 官方文档](https://docs.python.org/3.11/library/socket.html#socket.socket.settimeout)

正式样本先在内存保存，正常收尾后才写 requests.jsonl。用户中断或意外异常可能导致正式样本未落盘，目录会保留，不能把缺少完整报告的目录当作成功运行。本课不提供强制终止 Python 工作线程的功能。

## 7. 新文件与完整代码

在 `/home/rikanshin/VisionFlow/VisionFlow_learn/python/performance_observe.py` 中写入下面代码。复用的 video_client.py 与 vision_client.py 已在同一目录；导入它们不会执行被 main 保护的命令行入口。

建议先理解 observe_one 的计时与分类，再看 measure 的并发循环、latency_stats 的计算，最后把 main 串起来。这里只调用现有本地服务，不启动、停止或修改服务端。

~~~python
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import signal
import struct
import sys
import threading
import time

import cv2
import numpy as np

from video_client import ADDRESS, SOCKET_TIMEOUT, request_image
from vision_client import MAX_PAYLOAD


STATES = ("ok", "busy", "server_error", "client_error")


class LessonParser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, f"参数错误：{message}\n请使用 --help 查看用法。\n")


def bounded_int(low, high):
    def convert(text):
        try:
            value = int(text)
        except ValueError as error:
            raise argparse.ArgumentTypeError("必须输入整数") from error
        if not low <= value <= high:
            raise argparse.ArgumentTypeError(f"必须在 {low}～{high} 之间")
        return value
    return convert


def parse_args():
    parser = LessonParser(
        description=(
            "用法：python3 python/performance_observe.py "
            "--output-dir 新目录 --server-note 服务端条件说明 [其他参数]\n"
            "对现有本地服务端发送固定图片，分别统计成功与 busy。"
        ),
        usage=argparse.SUPPRESS,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        add_help=False,
    )
    options = parser.add_argument_group("参数")
    options.add_argument("-h", "--help", action="help", help="显示帮助后退出")
    options.add_argument("--image", type=Path, default=Path("samples/frame.jpg"),
                         help="固定 JPEG 图片，默认 samples/frame.jpg")
    options.add_argument("--requests", type=bounded_int(1, 1000), default=60,
                         help="正式请求总数，默认 60，上限 1000")
    options.add_argument("--concurrency", type=bounded_int(1, 8), default=1,
                         help="客户端同时等待请求的数量上限，默认 1，上限 8")
    options.add_argument("--warmup", type=bounded_int(0, 50), default=5,
                         help="正式计时前的顺序预热次数，默认 5")
    options.add_argument("--output-dir", required=True, type=Path,
                         help="必须不存在的新输出目录")
    options.add_argument("--server-note", required=True,
                         help="手动记录服务端参数；程序不会自动核实这段说明")
    args = parser.parse_args()
    if args.concurrency > args.requests:
        parser.error("并发数不得超过正式请求总数")
    if not args.server_note.strip():
        parser.error("服务端条件说明不能为空")
    return args


def require(condition, message):
    if not condition:
        raise ValueError(message)


def write_json(path, value):
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, ensure_ascii=False, indent=2, allow_nan=False)
        output.write("\n")


def observe_one(jpeg, width, height, request_id, worker, phase):
    state = "client_error"
    server_code = None
    error_text = None
    started = time.perf_counter()
    try:
        result = request_image(jpeg, request_id, width, height)
        if result["status"] == "ok":
            state = "ok"
        else:
            server_code = result["code"]
            state = "busy" if server_code == "busy" else "server_error"
    except (OSError, ValueError, struct.error) as error:
        error_text = f"{type(error).__name__}: {error}"
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    return {
        "phase": phase, "request_id": request_id, "client_worker": worker,
        "status": state, "latency_ms": elapsed_ms,
        "server_code": server_code, "error": error_text,
    }


def measure(jpeg, width, height, total, concurrency, first_id):
    next_id = first_id
    last_id = first_id + total
    claim_lock = threading.Lock()
    stop = threading.Event()

    def worker(index):
        nonlocal next_id
        local_rows = []
        while True:
            with claim_lock:
                if stop.is_set() or next_id >= last_id:
                    break
                request_id = next_id
                next_id += 1
            row = observe_one(jpeg, width, height, request_id, index, "measure")
            local_rows.append(row)
            if row["status"] in ("server_error", "client_error"):
                stop.set()
                break
        return local_rows

    # 整批时间包括线程池启动、请求调度、收集与线程池收尾。
    started = time.perf_counter()
    executor = ThreadPoolExecutor(max_workers=concurrency)
    rows = []
    try:
        futures = []
        for index in range(1, concurrency + 1):
            futures.append(executor.submit(worker, index))
        for future in as_completed(futures):
            rows.extend(future.result())
    finally:
        stop.set()
        old_handler = signal.signal(signal.SIGINT, signal.SIG_IGN)
        try:
            executor.shutdown(wait=True, cancel_futures=True)
        finally:
            signal.signal(signal.SIGINT, old_handler)
    elapsed_s = time.perf_counter() - started
    return rows, elapsed_s


def latency_stats(values):
    if not values:
        return None
    ordered = sorted(values)
    count = len(ordered)

    def percentile(fraction):
        # 最近秩定义：向上取整后的序号，从 1 起算。
        return ordered[math.ceil(fraction * count) - 1]

    return {
        "n": count,
        "min_ms": ordered[0],
        "mean_ms": sum(ordered) / count,
        "p50_ms": percentile(0.50),
        "p95_ms": percentile(0.95),
        "max_ms": ordered[-1],
    }


def main():
    args = parse_args()
    image_path = args.image.resolve()
    output_dir = args.output_dir.resolve()
    require(not output_dir.exists(), f"输出目录已存在，请换新目录：{output_dir}")
    with image_path.open("rb") as stream:
        jpeg = stream.read(MAX_PAYLOAD + 1)
    require(0 < len(jpeg) <= MAX_PAYLOAD, "JPEG 必须为 1 字节～16 MiB")
    require(jpeg.startswith(b"\xff\xd8"), "本课输入应为 JPEG 文件")
    frame = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
    require(frame is not None and frame.ndim == 3, "无法解码输入 JPEG")
    height, width = frame.shape[:2]
    require(0 < width <= 4096 and 0 < height <= 4096, "图片尺寸超出本课范围")
    del frame

    output_dir.mkdir(parents=True, exist_ok=False)
    config = {
        "schema_version": 1,
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "address": list(ADDRESS),
        "image": str(image_path),
        "image_sha256": hashlib.sha256(jpeg).hexdigest(),
        "jpeg_bytes": len(jpeg), "width": width, "height": height,
        "requests_planned": args.requests, "concurrency": args.concurrency,
        "warmup": args.warmup, "socket_timeout_s": SOCKET_TIMEOUT,
        "server_note_declared": args.server_note,
        "python": platform.python_version(), "opencv": cv2.__version__,
        "platform": platform.platform(),
        "load_model": "closed_loop_fixed_count",
        "latency_scope": "new_connection_request_response_and_validation",
        "batch_scope": "pool_setup_dispatch_requests_collect_shutdown",
        "percentile_method": "nearest_rank",
    }
    write_json(output_dir / "config.json", config)
    print(f"本轮输出目录：{output_dir}", flush=True)
    print(f"目标：{ADDRESS[0]}:{ADDRESS[1]}；固定 JPEG {len(jpeg)} 字节")
    print(f"并发上限 {args.concurrency}，正式请求 {args.requests}，预热 {args.warmup}")

    with (output_dir / "warmup.jsonl").open("x", encoding="utf-8") as output:
        for request_id in range(1, args.warmup + 1):
            row = observe_one(jpeg, width, height, request_id, 0, "warmup")
            output.write(json.dumps(row, ensure_ascii=False, allow_nan=False) + "\n")
            output.flush()
            require(row["status"] == "ok",
                    f"预热请求 {request_id} 为 {row['status']}，查看 warmup.jsonl")
    print("预热阶段结束，开始正式测量", flush=True)

    rows, wall_s = measure(jpeg, width, height, args.requests,
                           args.concurrency, args.warmup + 1)
    require(wall_s > 0, "整批计时异常")
    rows.sort(key=lambda row: row["request_id"])
    with (output_dir / "requests.jsonl").open("x", encoding="utf-8") as output:
        for row in rows:
            output.write(json.dumps(row, ensure_ascii=False, allow_nan=False) + "\n")

    counts = {state: sum(row["status"] == state for row in rows) for state in STATES}
    observed = len(rows)
    complete = (observed == args.requests and
                counts["server_error"] == 0 and counts["client_error"] == 0)
    valid_responses = counts["ok"] + counts["busy"] + counts["server_error"]
    stats = {
        state: latency_stats([row["latency_ms"] for row in rows
                              if row["status"] == state])
        for state in STATES
    }
    report = {
        "schema_version": 1,
        "measurement_status": "complete" if complete else "incomplete_or_error",
        "config": config,
        "requests_observed": observed, "counts": counts,
        "batch_elapsed_s": wall_s,
        "success_rps": counts["ok"] / wall_s,
        "valid_response_rps": valid_responses / wall_s,
        "busy_fraction_observed": counts["busy"] / observed if observed else None,
        "success_fraction_observed": counts["ok"] / observed if observed else None,
        "latency_by_status": stats,
    }
    write_json(output_dir / "report.json", report)
    print(f"正式记录 {observed}/{args.requests}；成功 {counts['ok']}，"
          f"busy {counts['busy']}，服务端错误 {counts['server_error']}，"
          f"客户端错误 {counts['client_error']}")
    print(f"整批耗时 {wall_s:.6f} 秒；成功吞吐 {report['success_rps']:.2f} 请求/秒；"
          f"有效响应吞吐 {report['valid_response_rps']:.2f} 响应/秒")
    busy_fraction = report["busy_fraction_observed"]
    print(f"busy 比例：{busy_fraction:.2%}" if busy_fraction is not None
          else "busy 比例：无样本")
    for state in STATES:
        value = stats[state]
        if value is not None:
            print(f"{state} 延迟：样本 {value['n']}，平均 {value['mean_ms']:.3f} ms，"
                  f"P50 {value['p50_ms']:.3f} ms，P95 {value['p95_ms']:.3f} ms，"
                  f"最大 {value['max_ms']:.3f} ms")
    print(f"测量状态：{report['measurement_status']}；报告：{output_dir / 'report.json'}")
    return 0 if complete else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("用户中断；正式样本可能尚未落盘，保留目录，勿当作完整测量。",
              file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, struct.error, cv2.error) as error:
        print(f"性能观察失败：{error}\n已创建的目录保留供排查。", file=sys.stderr)
        sys.exit(1)

~~~

## 8. 对照代码理解数据怎么流动

### 8.1 bounded_int：返回一个参数转换函数

bounded_int(1, 8) 返回内部 convert 函数。argparse 解析 --concurrency 时，将用户输入字符串交给它；convert 把字符串转为整数，再检查范围，错误以 argparse.ArgumentTypeError 告知解析器。

这使请求数、并发数、预热次数各有明确范围，且不重复写三套转换逻辑。并发还必须不大于正式请求总数。

### 8.2 observe_one：一次调用对应一条尝试记录

参数 jpeg、width、height 都已准备好；request_id 是这一尝试的编号；worker 是客户端工作循环编号，不能拿它推断服务端哪个 worker 在处理。

request_image 复用原有消息头、长度、类型、请求编号、JSON 状态与成功尺寸的校验。合法响应分为 ok、busy 或其他 server_error；网络或解析校验异常记录为 client_error。

错误发生时仍记录经过时间，但它只进入错误类别统计。例如连接超时不能混进“成功请求 P95”。

不捕获任意 Exception 来把代码错误伪装成正常测量记录。未预料到的程序错误会传播，线程池仍尝试收尾，但本次不生成成功测量报告。

### 8.3 measure：共享编号，分开的样本列表

next_id 定义在 measure 内部。嵌套 worker 想修改这个外层变量，所以使用 nonlocal next_id；它不是整个模块的 global 变量。

with claim_lock 覆盖：

~~~text
检查停止标志与编号上界 → 领取当前编号 → next_id 加一
~~~

取得编号后马上离开锁，再调用 observe_one。如果把网络调用也放进锁，C 条线程可能被变成串行请求。

正式编号范围是 [first_id, first_id + total)，右端不包含。预热为 5、N=60 时，first_id=6，上界 66，正式编号 6～65。

as_completed 看到的是某个工作循环整体结束；它取回该循环的多条 local_rows。汇总后按 request_id 排序，只方便查找，并不表示网络完成顺序。文件不包含完成时间轴，因此不能据记录行顺序画“实际完成次序”。

finally 中的 stop.set() 对成功路径也会执行，此时所有循环已完成；若中途有异常，它阻止仍在循环的线程继续领取新编号。已在途请求不会被撤回。

### 8.4 latency_stats：先按状态选样本，再排序

main 为每种状态选取自己的 latency_ms 列表，交给 latency_stats。函数对空列表返回 None，写成 JSON 的 null；非空时输出 n、最小值、平均值、最近秩 P50/P95 和最大值。

报告保留原始浮点数，控制台只格式化显示几位小数。不能根据显示为 0.000 ms 就推断实际耗时严格为零。

### 8.5 main：准备、预热、正式测量、保存

1. 有界读取原 JPEG，并确认能够解码；只使用本课已有本地素材，不重编码。
2. 创建新目录，保存配置和输入指纹。
3. 顺序预热，逐行写 warmup.jsonl；任何一项不是 ok 就不开始正式阶段。
4. 并发测量，线程池完成后返回 rows 和 wall_s。
5. 将正式记录按请求编号写入 requests.jsonl，分别统计状态。
6. 保存 report.json，再打印结果并返回测量退出码。

输出包括：

~~~text
outputs/perf_c1_01/
    config.json       # 参数、输入指纹、环境与计时定义
    warmup.jsonl      # 独立的预热记录
    requests.jsonl    # 正式测量记录
    report.json       # 分类统计、吞吐、比例与完成状态
~~~

这些 requests.jsonl 是本课性能样本格式，不是 11-1 的视频记录：没有 frame_index 或完整算法结果，不能传给 video_replay.py。

本课将 server_error 与 client_error 视为需要分析的异常，可能得到少于计划数的记录；它们保留在报告中，而不是删除慢请求或错误样本后只展示好看的数字。

## 9. 由你完成三组可控实验

### 9.1 终端 A：启动已有服务端

在学习工程根目录，用现有 visionflow 环境运行：

~~~bash
./build/vision_pool_server
~~~

看到监听提示后，确认其中“观察延迟 0 毫秒”。本课主实验不要加 --demo-delay-ms；人为延迟会改变你要测量的系统，不能混入真实处理速度的比较。

保持此服务端运行，不同时启动旧监管器或其他服务端入口，暂不运行其他客户端。

服务端固定 127.0.0.1:19090，测量脚本也复用该地址。这里只测自己已有的本地服务。

### 9.2 终端 B：依次测量 C=1、2、6

在学习工程根目录依次运行，上一组结束后再运行下一组。每次 echo $? 紧跟该测量命令。

~~~bash
python3 python/performance_observe.py \
  --concurrency 1 --requests 60 --warmup 5 \
  --server-note "workers=2,queue=2,delay_ms=0" \
  --output-dir outputs/perf_c1_01
echo $?
~~~

~~~bash
python3 python/performance_observe.py \
  --concurrency 2 --requests 60 --warmup 5 \
  --server-note "workers=2,queue=2,delay_ms=0" \
  --output-dir outputs/perf_c2_01
echo $?
~~~

~~~bash
python3 python/performance_observe.py \
  --concurrency 6 --requests 60 --warmup 5 \
  --server-note "workers=2,queue=2,delay_ms=0" \
  --output-dir outputs/perf_c6_01
echo $?
~~~

默认图片就是已有 samples/frame.jpg。如果某个目录已存在，只改成新的编号，不清空旧目录。

这三组都是新一课的测量，不是要求重跑旧课验收。主要观察三组各一次即可；有报错先反馈，不为凑结果反复运行。

### 9.3 预期结构，不预设性能数值

正常完成时应看到：

~~~text
并发上限 C，正式请求 60，预热 5
预热阶段结束，开始正式测量
正式记录 60/60；成功 ...，busy ...，服务端错误 0，客户端错误 0
整批耗时 ... 秒；成功吞吐 ... 请求/秒；有效响应吞吐 ... 响应/秒
busy 比例：...%
ok 延迟：样本 ...，平均 ... ms，P50 ... ms，P95 ... ms，最大 ... ms
测量状态：complete；报告：.../report.json
~~~

有 busy 时还会打印 busy 的独立延迟；某状态没有样本则不打印该组，报告内为 null。退出码预期 0，但你仍要读成功数和 busy 比例。

不规定 C=6 必须出现 busy，不规定成功／拒绝数量，也不规定 C=2 一定比 C=1 快。有 busy 或没有 busy 都先按真实证据分析，不为了出现拒绝而改本课主实验的服务端延迟。

你以前的六请求观察脚本与本课不同：以前集中发送六项并观察人为延迟下的返回顺序；本课是工作循环不断补新请求，正式总数 60。不要套用旧课“约四个成功、两个 busy”的预期。

### 9.4 怎样整理比较

从三份控制台输出和 report.json 整理：

| 并发上限 | 正式样本数 | 成功数 | busy 比例 | 成功请求/秒 | 成功 P50 | 成功 P95 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 实测 | 实测 | 实测 | 实测 | 实测 | 实测 |
| 2 | 实测 | 实测 | 实测 | 实测 | 实测 | 实测 |
| 6 | 实测 | 实测 | 实测 | 实测 | 实测 | 实测 |

没有成功样本时填“无样本”，不要填 0 毫秒。完整报告会保留输入指纹和各组样本数，便于核对。

你不必先自行算完表格再反馈，直接贴三组完整控制台输出、紧接着的退出码，以及服务端最初的监听参数即可，老师会一起分析。不要把数百行逐连接日志全部贴来；出错时再定位相关部分。

观察完成后，由你回到终端 A 按 Ctrl+C 正常停止。这里不额外开展一次停止功能验收。

## 10. 如何解释结果，避免过度结论

先检查 measurement_status、样本数和错误数，再比较成功吞吐与成功延迟。

可能出现：

- C=2 成功吞吐增加：说明该批条件下更多并发帮助重叠了部分等待或计算，但不能只凭这一点定位唯一瓶颈。
- C=6 有效响应吞吐增加，同时 busy 比例增加：可能主要是快速拒绝增加，不能称为成功处理能力提高。
- C=6 成功吞吐变化不大，成功 P95 变大：可能有更多等待或资源争用，需要进一步证据，不能直接断言算法变慢。
- 三组差别很小或次序反常：短批次、输入简单、客户端开销、日志输出和系统调度都可能影响；先保留结果，不编造原因。
- 成功样本很少：该组 P95 只是少量样本的描述，不适合支撑稳定尾延迟判断。

本课三组单轮结果用于学习测量口径。若以后要作优化决策，再固定条件、多轮重复并适当交错运行顺序，记录波动；不能挑最好的一次当作普遍结论。本课不强制追加这些实验。

没有服务端内部耗时、CPU／队列等数据时，不从客户端延迟倒推出算法纯耗时，也不据此宣布服务器最高容量。闭环客户端对服务变慢会自行降速，无法代表所有外部到达模式。

### 10.1 本次实测分析（2026-10-14）

下面是学生自行运行三组实验的结果，老师只读核对已有样本和报告，没有重新执行测量：

| C | 正式 ok / busy | 整批秒数 | 成功请求/秒 | busy 比例 | 成功平均 ms | 成功 P50 ms | 成功 P95 ms |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 60 / 0 | 0.345821 | 173.50 | 0% | 5.743 | 5.697 | 6.777 |
| 2 | 60 / 0 | 0.202237 | 296.68 | 0% | 6.673 | 6.470 | 10.655 |
| 6 | 20 / 40 | 0.057499 | 347.83 | 66.67% | 11.586 | 11.846 | 14.977 |

三组均 measurement_status=complete、无其他 server_error/client_error；每组五条预热记录均 ok，正式编号均为 6～65。记录的输入指纹、JPEG 大小和环境字段一致，统计与已有原始记录相符。服务端参数来自学生的 server_note_declared，本次未单独提供启动行，不能将其表述为老师独立确认的服务端运行配置。

**C=1 到 C=2：同样成功 60 项，整批用时减少，成功吞吐约增加 71%。** 但成功平均延迟和 P95 都上升。请求区间可以重叠，所以“整体完成得更多”和“每项都更快”不是一回事；不能把吞吐提升解释为算法单次计算变快。

**C=6：1043.50/s 是含拒绝的有效响应吞吐，成功吞吐是 347.83/s。** 40 项快速 busy 不能算作完成了图片处理。相对 C=2，这一短批次的成功吞吐约增加 17.2%，代价包括 66.67% 被拒绝、成功延迟更高。不能据此选出普遍最优并发配置。

C=6 的整批时间约 57.5 毫秒，比另外两组短很多，但它只成功处理 20 项，而另外两组成功 60 项。快速拒绝也消耗固定总请求额度，因此这不是相同成功工作量的耗时比较。未交回额度上限 4 限制同时占用的任务，不限制一整轮只能累计成功 4 项。

如果把 C=6 的两类延迟混在一起平均：

~~~text
(20 × 11.586119... + 40 × 1.900806...) / 60
≈ 5.129 ms
~~~

这个混合数甚至低于 C=1 的成功平均 5.743 ms，容易让人误以为 C=6 的图片处理更快。实际上 C=6 成功请求平均是 11.586 ms；快速失败样本拉低了混合值。

分位数也要看样本数：C=1/2 的成功 P95 来自 60 个样本中的第 57 个；C=6 来自 20 个成功样本中的第 19 个。本轮最长批次仍不足半秒，三组单次观察只支持这次固定条件下的描述，不证明长期尾延迟、稳态容量或瓶颈位置。

学生未贴各组紧接命令的 echo $?，不能把 report 的 complete 倒填为实际看到的 shell 退出码；已有完整报告和样本足以确认本课主要实践，不要求补跑。本次只接续集中理解检查。

## 11. 常见定位入口

| 现象 | 先检查 |
| --- | --- |
| 预热 client_error，连接被拒绝 | 服务端是否已监听、是否使用同一 127.0.0.1:19090 |
| 预热 busy | 是否还有其他请求、服务器是否处于持续负载；先反馈，不自动重试 |
| 预热失败但没有 report.json | 正式测量没有开始，查看 config.json 和 warmup.jsonl |
| 正式记录少于 60 | server_error/client_error 和错误详情；已停止领取新任务 |
| C=6 busy 很多、成功 P95 样本很少 | 闭环快速拒绝和固定请求总量的影响，不把小样本当稳态 |
| 报告返回 complete，但成功数不足 60 | busy 是允许观测的结果；complete 不代表全部成功 |
| P50 与某工具的 median 不同 | 分位数定义；本课 nearest_rank，不做中间两值平均 |
| 相加请求耗时远大于整批时间 | 并发请求的等待区间发生重叠 |
| Ctrl+C 后仍等一会儿 | 线程池等待已运行请求结束；取消待执行任务不等于强杀线程 |
| 中断后没有正式样本或报告 | 正式数据在内存中，正常收尾后才写盘，不视为完整测量 |
| 不同组数值差异难解释 | 输入指纹、服务端声明、预热、并发、日志和系统负载是否可比 |

## 12. 集中理解检查

主要实践与结果讨论后，一次回答：

1. 本课 latency_ms 包含哪些阶段，不包含哪些阶段？与 11-1 的耗时有何区别？为什么各请求耗时之和不能代替整批时间？
2. 若正式 60 项中 40 项 ok、20 项 busy，整批 2 秒，成功吞吐、有效响应吞吐和 busy 比例分别是多少？为什么不能混在一起算“成功平均延迟”？返回码 0 是否表示全部成功？
3. 最近秩算法对 1～20 毫秒这 20 个样本，P50/P95 各是多少？只有三项成功或完全没有成功时，应怎样理解报告？
4. C=6 与服务端两条工作线程是什么关系？为什么线程池只提交 C 个工作循环？领取编号的锁为何不能包住网络调用？busy 和 client_error 分别怎样影响下一项请求？
5. 为什么预热单独记录？为什么要固定输入、环境和服务端条件？这套“收到响应再补请求”的闭环实验，能否证明固定到达率下的容量、算法纯耗时或长期尾延迟？

## 12.1 五点直接解答（2026-10-14）

学生明确要求“帮我回答，然后进行下一环节”。以下由老师直接解释，不记作学生独立答对；结合已经通过的实现与三组主要实践，按要求收尾 11-4，并进入项目回顾与收尾，不重复原题或要求补跑。

1. **单项计时覆盖一次新连接请求的客户端观察过程。** 包含协议消息构造、建立 TCP 连接、发送、服务端收包／排队／处理、接收响应、JSON 和编号／尺寸等校验、关闭连接及结果分类。不包含输入 JPEG 文件读取、预先解码、预热、领取编号前的锁等待、最终样本与报告落盘；本课直接复用已有 JPEG 字节，正式阶段不做 JPEG 编码。11-1 的 elapsed_ms 则从逐帧 JPEG 编码前开始，两课计时范围不同。并发请求区间会重叠，相加得到的是多个请求耗时的总和，不是实际经过的整批时间；本课整批时间还包含线程池启动、调度、汇总与收尾。
2. **1043.50/s 计合法响应，347.83/s 只计成功。** C=6 的整批实际约 0.057498944 秒：60 个合法响应除以该时间约为 1043.50/s；其中 20 个 ok 除以同一时间约为 347.83/s；40/60=66.67% 被 busy 拒绝。complete 的约定是正式计划数全部完成，且只出现 ok 或 busy，报告写入成功，不是要求每项都 ok；因此 complete 可以与 40 次 busy 同时出现，代码正常返回 0 的含义也是测量完成。这个代码约定不倒填为本次实际展示过的 shell 退出码。讲义原手算题若为 60 项、40 ok、20 busy、整批 2 秒，则成功吞吐 20/s、有效响应吞吐 30/s、busy 比例 33.33%。不能把 busy 延迟混入成功平均延迟：本轮混合均值约 5.129 ms，会掩盖成功均值 11.586 ms。
3. **P95 的位置由所属类别的样本数决定。** C=6 只有 20 个成功样本，最近秩序号 ceil(0.95×20)=19，因此成功 P95 是从小到大排序后的第 19 项，约 14.977 ms；不是把 60 项全混在一起排序，也不是第 95 项。C=1/2 各有 60 项成功，所以 P95 取第 57 项。只有 3 项成功时 P95 取第 3 项，即这三个样本的最大值；它只是小样本描述，不能证明长期尾延迟。没有成功样本时该组统计写 null，不能填 0 ms。原例中样本为 1～20 ms 时，最近秩 P50=10 ms、P95=19 ms，P50 不采用中间两项平均。
4. **客户端并发上限不改变服务端计算线程数。** C=6 表示客户端至多六条请求循环各执行一个在途请求；服务端仍是现有两条计算线程，其他任务可能排队或被拒绝。线程池仅提交 C 个循环，各循环做完一次再领取新编号，避免把 N 个独立请求全部提交到客户端任务队列。锁只保护“检查停止／上界、领取编号、递增”这一小段；若锁包含网络收发，一个线程等待响应时其他线程连请求都不能开始，可能把并发退化为串行。busy 是预期过载反馈，会记录后领取新的请求编号，不重试原请求；client_error（以及非 busy 的 server_error）设置 stop，停止领取后续新项，已领取并执行中的请求仍会收尾，所以错误发生后仍可能看到其他记录。stop 不等于撤销已提交的服务端计算。
5. **预热与控制条件使测量含义更清楚，但不自动获得容量结论。** 五次预热可能减少部分首次初始化和缓存影响，单独保存才能让正式 N、延迟分布及整批时间不混入这部分开销；它不保证系统已经进入稳态，也不测冷启动。同样输入字节、环境、服务端参数和日志方式，才能尽量把差异集中到并发变化；本轮指纹和环境字段相同，但服务端说明是人工声明，不是客户端独立读取配置。闭环客户端必须等响应才补请求，服务变慢时发起速度也会下降，快速 busy 又会迅速消耗有限请求总量。因此这三组短批次只能描述本次条件下的现象，不能证明固定外部到达率下的稳定容量，也不能把包含网络、等待和校验的时间称为算法纯耗时，不能从 20 或 60 个样本推断长期尾延迟。

## 13. 当前状态与接续边界

2026-10-14（11-4、模块 11 与原定基础路线按要求收尾）：学生明确要求“帮我回答，然后进行下一环节”。老师直接解释了计时范围及并发时间重叠、1043.50 与 347.83 两种吞吐及 complete 的含义、20 个成功样本的最近秩 P95 取第 19 项、客户端与服务端线程数的区别及编号锁／错误停止、预热和闭环测量的能力边界。完整解答见 11-4 第 12.1 节，不记为学生独立答对。结合已完成的实现和三组主要实践，按要求收尾 11-4，并进入既定的项目回顾与收尾；新增 lessons/lesson11/11-5-project-wrap-up.md，整理架构、源码入口、编号关联、证据、性能表述和能力边界，没有新增代码或运行要求。模块 1～11 原定基础教学路线按教学范围收尾；这不等于生产就绪、所有异常验证通过、算法精度已证明或与上一级原项目完全等价。此前未提供的可视观察、部分 shell 退出码和服务端独立配置证据不倒填，不为收尾要求补跑。本次没有自动开启新技术模块、优化或压测；后续围绕学生提出的具体目标接续，新主题再询问熟悉程度，不重教旧课。老师仅维护教学文档，未修改源码、CMake 或结果文件，未配置、构建、运行或测试。

下一环节已整理为 [11-5 项目回顾与收尾](11-5-project-wrap-up.md)。下方为此前记录，以本段最新状态为准。

2026-10-14（11-4 实现与三组主要实践通过，集中理解检查待完成）：学生已创建 python/performance_observe.py，老师只读核对与讲义完整代码一致（忽略换行格式及末尾空白）。学生完成 outputs/perf_c1_01、perf_c2_01、perf_c6_01 三组，均正式 60 项、顺序预热 5，输入 JPEG 16045 字节、640×360，服务端声明 workers=2,queue=2,delay_ms=0；报告均 complete。C=1：60 ok/0 busy，整批 0.345821 秒，成功吞吐 173.50/s，成功平均/P50/P95 为 5.743/5.697/6.777 ms。C=2：60 ok/0 busy，0.202237 秒，296.68/s，6.673/6.470/10.655 ms。C=6：20 ok/40 busy，0.057499 秒，成功吞吐 347.83/s、有效响应吞吐 1043.50/s、busy 66.67%；成功平均/P50/P95 为 11.586/11.846/14.977 ms，busy 为 1.901/1.670/3.472 ms。三组均无其他服务端错误或客户端错误。老师只读核对 report.json、requests.jsonl、warmup.jsonl：输入 SHA-256 一致，Python 3.11.16/OpenCV 4.11.0/WSL2 环境字段一致；每组预热编号 1～5 全 ok、正式编号 6～65，计数、均值、最近秩分位数和成功吞吐与已有记录一致。没有执行测量程序、解码素材或运行测试。学生未展示每组 echo $?、服务端启动参数行或停止输出，不倒填 shell 退出码或独立核实的服务端配置，也不要求为此重跑。C2 本批成功吞吐约为 C1 的 1.71 倍但延迟更高；C6 相对 C2 的成功吞吐约增 17.2%，同时三分之二被拒绝、成功样本仅 20，不能当作等量成功工作更快或稳定容量证明。将 C6 两类耗时混算约 5.129 ms 会掩盖成功平均 11.586 ms，讲义已补入分析。当前接续五点理解检查，不重复熟悉程度或要求重跑；模块 11 尚未整体收尾，不提前改算法、加压或自动优化。老师仅维护教学文档，源码和既有结果未改。

以下保留此前准备记录，以本段最新状态为准。

2026-10-14：11-3 的实现与主要实践已通过，五点由老师直接解释后，学生要求“进行下一步”，据此收尾 11-3，不记作学生独立答对，也不重复要求运行。此前未提供的可视与异常分支证据不倒填。

11-4 熟悉程度已确认为“完全不熟悉，需要从头讲”，不再询问。python/performance_observe.py 已由学生亲手新建，实现与三组主要实践通过，实测分析见第 10.1 节；五点由老师按要求直接解答后，按学生要求进入项目回顾并收尾。老师仅维护教学资料，没有创建主要源码、配置、构建、运行或测试。

本课是固定 JPEG、本地 TCP、有限总量的闭环请求观察。默认 N=60、预热 5，主要比较 C=1/2/6；分别记录成功、busy、其他服务端错误和客户端错误，不预设吞吐、分位数或拒绝数量。它不等于稳态容量测试，不包含自动优化、服务端内部剖析、固定到达率发压或生产负载复现。

模块 11 的回顾与收尾已按学生要求完成，见 11-5；不将教学路线收尾等同于整个项目性能达标。API 细节对照所链接的官方文档，所有示例运行与测量值以学生反馈为准。
