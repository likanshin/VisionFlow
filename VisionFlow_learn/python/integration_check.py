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

