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

