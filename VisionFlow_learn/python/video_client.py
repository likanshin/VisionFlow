import argparse
import json
import math
from pathlib import Path
import socket
import struct
import sys
import time

import cv2

from vision_client import HEADER, MAX_PAYLOAD, MAX_RESPONSE_BODY, recv_exact


ADDRESS = ("127.0.0.1", 19090)
SOCKET_TIMEOUT = 5.0
JPEG_QUALITY = 90


class LessonParser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, f"参数错误：{message}\n请使用 --help 查看用法。\n")


def positive_int(text):
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("必须输入整数") from error
    if value <= 0:
        raise argparse.ArgumentTypeError("必须是正整数")
    return value


def parse_args():
    parser = LessonParser(
        description=(
            "用法：python3 python/video_client.py --input 视频路径 "
            "--output 结果路径 [--stride 3] [--max-frames 5]\n"
            "顺序抽帧，逐帧请求服务端，并将结果保存为 JSONL。"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        usage=argparse.SUPPRESS,
        add_help=False,
    )
    options = parser.add_argument_group("参数")
    options.add_argument("-h", "--help", action="help", help="显示帮助后退出")
    options.add_argument("--input", required=True, type=Path, help="本地视频文件")
    options.add_argument("--output", required=True, type=Path, help="新建 JSONL 文件")
    options.add_argument(
        "--stride", type=positive_int, default=3, help="每隔多少帧选一帧，默认 3"
    )
    options.add_argument(
        "--max-frames", type=positive_int, default=5,
        help="最多尝试处理多少个选中帧，默认 5",
    )
    args = parser.parse_args()
    if args.max_frames > (1 << 64) - 1:
        parser.error("--max-frames 超出请求编号的 64 位范围")
    return args


def reject_nonfinite(text):
    raise ValueError(f"响应包含非标准 JSON 数值：{text}")


def request_image(jpeg, request_id, width, height):
    if not 0 < len(jpeg) <= MAX_PAYLOAD:
        raise ValueError("编码后的 JPEG 必须在 1 字节到 16 MiB 之间")

    header = HEADER.pack(b"VFLW", 1, 1, request_id, len(jpeg), 0)
    with socket.create_connection(ADDRESS, timeout=SOCKET_TIMEOUT) as connection:
        connection.sendall(header + jpeg)
        response_header = recv_exact(connection, HEADER.size)
        magic, version, kind, response_id, length, flags = HEADER.unpack(
            response_header
        )
        if magic != b"VFLW" or version != 1 or flags != 0:
            raise ValueError("响应消息头格式不正确")
        if kind not in (2, 3):
            raise ValueError("响应类型必须是结果或错误")
        if response_id != request_id:
            raise ValueError("响应头中的请求编号不匹配")
        if not 0 < length <= MAX_RESPONSE_BODY:
            raise ValueError("响应消息体长度超出允许范围")

        body = recv_exact(connection, length)
        result = json.loads(
            body.decode("utf-8"), parse_constant=reject_nonfinite
        )
        if not isinstance(result, dict):
            raise ValueError("响应 JSON 必须是对象")
        if type(result.get("request_id")) is not int:
            raise ValueError("响应 JSON 请求编号必须是整数")
        if result["request_id"] != request_id:
            raise ValueError("响应 JSON 请求编号不匹配")
        expected_status = "ok" if kind == 2 else "error"
        if result.get("status") != expected_status:
            raise ValueError("响应消息类型与 JSON 状态不一致")
        if kind == 2:
            if (type(result.get("width")) is not int or
                    type(result.get("height")) is not int or
                    result["width"] != width or result["height"] != height):
                raise ValueError("响应图像尺寸与发送帧不一致")
        elif not isinstance(result.get("code"), str):
            raise ValueError("错误响应缺少字符串类型的 code")
        return result


def main():
    args = parse_args()
    input_path = args.input.resolve()
    output_path = args.output.resolve()
    if not input_path.is_file():
        raise ValueError(f"输入不是现有视频文件：{input_path}")
    if output_path.exists():
        raise FileExistsError(f"结果文件已存在，请换一个输出文件名：{output_path}")

    capture = cv2.VideoCapture(str(input_path))
    try:
        if not capture.isOpened():
            raise ValueError(f"无法打开视频：{input_path}")
        raw_fps = float(capture.get(cv2.CAP_PROP_FPS))
        fps = raw_fps if math.isfinite(raw_fps) and raw_fps > 0 else None
        print(f"输入视频：{input_path}")
        print(f"视频标称帧率：{fps if fps is not None else '未知'}")
        print(
            f"抽帧间隔：{args.stride}，最多处理选中帧：{args.max_frames}"
        )

        output_path.parent.mkdir(parents=True, exist_ok=True)
        decoded = 0
        processed = 0
        successes = 0
        server_errors = 0
        client_errors = 0
        # x：创建新文件；若已存在就失败，避免覆盖以前的记录。
        with output_path.open("x", encoding="utf-8", newline="\n") as output:
            while processed < args.max_frames:
                readable, frame = capture.read()
                if not readable:
                    if decoded == 0:
                        raise ValueError("视频没有读出任何可用帧")
                    print("未再读到视频帧：可能到达末尾或发生解码失败")
                    break
                if frame is None or frame.size == 0:
                    raise ValueError("视频读取返回了空图像")

                frame_index = decoded
                decoded += 1
                if frame_index % args.stride != 0:
                    continue

                request_id = processed + 1
                height, width = frame.shape[:2]
                record = {
                    "schema_version": 1,
                    "input_video": str(input_path),
                    "source_fps": fps,
                    "frame_index": frame_index,
                    "estimated_video_time_ms": (
                        round(frame_index * 1000.0 / fps, 3)
                        if fps is not None else None
                    ),
                    "request_id": request_id,
                    "width": int(width),
                    "height": int(height),
                    "jpeg_bytes": None,
                    "elapsed_ms": None,
                    "status": None,
                    "result": None,
                    "error": None,
                }
                started = time.perf_counter()
                try:
                    encoded_ok, encoded = cv2.imencode(
                        ".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY]
                    )
                    if not encoded_ok:
                        raise ValueError("当前帧 JPEG 编码失败")
                    jpeg = encoded.tobytes()
                    record["jpeg_bytes"] = len(jpeg)
                    result = request_image(jpeg, request_id, int(width), int(height))
                    record["result"] = result
                    if result["status"] == "ok":
                        record["status"] = "ok"
                        successes += 1
                    else:
                        record["status"] = "server_error"
                        server_errors += 1
                except (OSError, ValueError, struct.error, cv2.error) as error:
                    record["status"] = "client_error"
                    record["error"] = {
                        "type": type(error).__name__,
                        "message": str(error),
                    }
                    client_errors += 1

                record["elapsed_ms"] = round(
                    (time.perf_counter() - started) * 1000.0, 3
                )
                output.write(
                    json.dumps(record, ensure_ascii=False, allow_nan=False) + "\n"
                )
                output.flush()
                processed += 1
                print(
                    f"帧 {frame_index} → 请求 {request_id}："
                    f"{record['status']}，约 {record['elapsed_ms']:.2f} 毫秒"
                )
                if record["status"] == "client_error":
                    print("本帧发生客户端错误，记录后停止，不自动重试")
                    break

        if processed == args.max_frames:
            print("已达到选中帧处理上限")
        print(
            f"汇总：读取帧 {decoded}，记录 {processed}，成功 {successes}，"
            f"服务端错误 {server_errors}，客户端错误 {client_errors}"
        )
        print(f"结果文件：{output_path}")
        return 0 if processed > 0 and server_errors == 0 and client_errors == 0 else 1
    finally:
        capture.release()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("用户中断；已写入的记录保留，当前帧可能尚未形成完整记录", file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, struct.error, cv2.error) as error:
        print(f"视频客户端失败：{error}", file=sys.stderr)
        sys.exit(1)
