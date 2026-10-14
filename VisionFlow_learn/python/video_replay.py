import argparse
import json
import math
from pathlib import Path
import sys

import cv2


MAX_LOG_BYTES = 8 * 1024 * 1024
MAX_LINE_BYTES = 128 * 1024
MAX_RECORDS = 10000
MAX_OUTPUT_FRAMES = 10000
MAX_LINES = 256
MAX_SIDE = 4096


class LessonParser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, f"参数错误：{message}\n请使用 --help 查看用法。\n")


def checked_fps(text):
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("帧率必须是数字") from error
    if not math.isfinite(value) or not 0 < value <= 240:
        raise argparse.ArgumentTypeError("帧率必须大于 0 且不超过 240")
    return value


def parse_args():
    parser = LessonParser(
        description=(
            "用法：python3 python/video_replay.py --records 记录路径 "
            "--output-dir 新输出目录 [--fps 输出帧率]\n"
            "读取 JSONL 和原视频，离线绘制对应帧的检测结果。"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        usage=argparse.SUPPRESS,
        add_help=False,
    )
    options = parser.add_argument_group("参数")
    options.add_argument("-h", "--help", action="help", help="显示帮助后退出")
    options.add_argument("--records", required=True, type=Path, help="11-1 JSONL 文件")
    options.add_argument(
        "--output-dir", required=True, type=Path, help="必须不存在的新输出目录"
    )
    options.add_argument(
        "--fps", type=checked_fps, help="可选；显式指定输出帧率，否则读取源视频帧率"
    )
    return parser.parse_args()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def reject_nonfinite(text):
    raise ValueError(f"不接受非标准 JSON 数值：{text}")


def is_integer(value, minimum=0):
    return type(value) is int and value >= minimum


def validate_record(item):
    require(isinstance(item, dict), "每行必须是 JSON 对象")
    require(type(item.get("schema_version")) is int and
            item["schema_version"] == 1, "只支持 schema_version=1")
    require(is_integer(item.get("frame_index")), "frame_index 必须是非负整数")
    require(item["frame_index"] < MAX_OUTPUT_FRAMES,
            "帧下标超过本课输出范围上限")
    require(is_integer(item.get("request_id"), 1) and
            item["request_id"] <= (1 << 64) - 1, "request_id 必须是正的 64 位整数")
    require(is_integer(item.get("width"), 1) and
            is_integer(item.get("height"), 1), "尺寸必须是正整数")
    require(item["width"] <= MAX_SIDE and item["height"] <= MAX_SIDE,
            "宽高超过本课单边 4096 像素上限")
    source_text = item.get("input_video")
    require(isinstance(source_text, str) and bool(source_text),
            "input_video 必须是非空字符串")
    require(Path(source_text).is_absolute(), "input_video 必须是绝对路径")

    state = item.get("status")
    require(state in ("ok", "server_error", "client_error"), "未知记录状态")
    result = item.get("result")
    error = item.get("error")
    if state == "client_error":
        require(result is None, "client_error 的 result 必须为 null")
        require(isinstance(error, dict), "client_error 必须包含 error 对象")
        require(isinstance(error.get("type"), str) and
                isinstance(error.get("message"), str), "客户端错误详情不完整")
        return

    require(error is None, "有效响应记录的 error 必须为 null")
    require(isinstance(result, dict), "响应必须是 JSON 对象")
    require(type(result.get("request_id")) is int and
            result["request_id"] == item["request_id"], "内外请求编号不一致")
    if state == "server_error":
        require(result.get("status") == "error" and
                isinstance(result.get("code"), str), "服务端错误格式不正确")
        return

    require(result.get("status") == "ok", "成功记录的响应状态不正确")
    require(type(result.get("width")) is int and
            type(result.get("height")) is int and
            result["width"] == item["width"] and
            result["height"] == item["height"], "内外图像尺寸不一致")
    lines = result.get("lines")
    require(isinstance(lines, list) and len(lines) <= MAX_LINES,
            "成功响应的 lines 必须是本课上限内的数组")
    for line in lines:
        require(isinstance(line, list) and len(line) == 4,
                "每条线段必须是四个坐标")
        require(all(is_integer(value) for value in line),
                "线段坐标必须是非负整数")
        x1, y1, x2, y2 = line
        require(x1 < item["width"] and x2 < item["width"] and
                y1 < item["height"] and y2 < item["height"],
                "线段坐标超出图像边界")


def load_records(path):
    records = []
    total_bytes = 0
    source_path = None
    size = None
    previous_frame = -1
    previous_request = 0
    with path.open("rb") as stream:
        line_number = 0
        while True:
            raw = stream.readline(MAX_LINE_BYTES + 1)
            if not raw:
                break
            line_number += 1
            total_bytes += len(raw)
            require(len(raw) <= MAX_LINE_BYTES,
                    f"第 {line_number} 行超过长度上限")
            require(total_bytes <= MAX_LOG_BYTES, "记录文件超过 8 MiB 上限")
            require(len(records) < MAX_RECORDS, "记录数量超过本课上限")
            try:
                require(bool(raw.strip()), "不接受空行")
                item = json.loads(
                    raw.decode("utf-8"), parse_constant=reject_nonfinite
                )
                validate_record(item)
                source = Path(item["input_video"]).resolve()
                current_size = (item["width"], item["height"])
                if source_path is None:
                    source_path = source
                    size = current_size
                require(source == source_path, "记录混入了不同源视频")
                require(current_size == size, "记录中的图像尺寸发生变化")
                require(item["frame_index"] > previous_frame,
                        "帧下标必须严格递增，不接受重复或乱序")
                require(item["request_id"] > previous_request,
                        "本课请求编号必须严格递增")
                previous_frame = item["frame_index"]
                previous_request = item["request_id"]
                records.append(item)
            except (ValueError, OSError, RecursionError) as error:
                raise ValueError(f"第 {line_number} 行：{error}") from error
    require(bool(records), "记录文件为空")
    return records, source_path, size


def draw_frame(frame, frame_index, record):
    canvas = frame.copy()
    color = (180, 180, 180)
    state = "unrecorded"
    request_text = "-"
    if record is not None:
        state = record["status"]
        request_text = str(record["request_id"])
        color = (0, 220, 0) if state == "ok" else (0, 0, 255)
        if state == "ok":
            for x1, y1, x2, y2 in record["result"]["lines"]:
                cv2.line(canvas, (x1, y1), (x2, y2), color, 2, cv2.LINE_AA)

    # OpenCV 内置字体适合这些 ASCII 字段名；控制台说明使用中文。
    cv2.rectangle(canvas, (0, 0), (canvas.shape[1] - 1, 30), (0, 0, 0), -1)
    label = f"frame_index={frame_index} request_id={request_text} status={state}"
    cv2.putText(
        canvas, label, (8, 21), cv2.FONT_HERSHEY_SIMPLEX,
        0.45, color, 1, cv2.LINE_AA,
    )
    return canvas


def main():
    args = parse_args()
    records_path = args.records.resolve()
    output_dir = args.output_dir.resolve()
    records, source_path, size = load_records(records_path)
    require(source_path.is_file(), f"源视频不存在：{source_path}")
    require(not output_dir.exists(), f"输出目录已存在，请换一个新目录：{output_dir}")
    width, height = size
    require(width % 2 == 0 and height % 2 == 0,
            "本课 MJPG 输出要求偶数宽高，不自动裁剪或缩放坐标")

    capture = cv2.VideoCapture(str(source_path))
    writer = None
    created_output = False
    try:
        require(capture.isOpened(), f"无法打开源视频：{source_path}")
        source_fps = float(capture.get(cv2.CAP_PROP_FPS))
        output_fps = args.fps if args.fps is not None else source_fps
        require(math.isfinite(output_fps) and 0 < output_fps <= 240,
                "源帧率不可用或超出本课范围，请用 --fps 显式指定")
        last_frame = records[-1]["frame_index"]
        print(f"源视频：{source_path}")
        print(f"记录数：{len(records)}，回放帧范围：0～{last_frame}")
        print(f"输出尺寸：{width}×{height}，输出 FPS：{output_fps}")
        print("离线回放：不连接服务端，不重新执行检测")
        if args.fps is not None:
            print("已显式指定输出 FPS，播放速度可能与源视频不同")

        output_dir.mkdir(parents=True, exist_ok=False)
        created_output = True
        video_path = output_dir / "replay.avi"
        writer = cv2.VideoWriter(
            str(video_path), cv2.VideoWriter_fourcc(*"MJPG"),
            output_fps, (width, height),
        )
        require(writer.isOpened(), "无法创建 MJPG/AVI 回放视频")

        record_pos = 0
        snapshots = 0
        unrecorded_preview = False
        counts = {"ok": 0, "server_error": 0, "client_error": 0}
        for frame_index in range(last_frame + 1):
            readable, frame = capture.read()
            require(readable and frame is not None and frame.size > 0,
                    f"源视频在帧 {frame_index} 读取失败，尚未覆盖全部记录")
            require(frame.ndim == 3 and frame.shape[2] == 3 and
                    (frame.shape[1], frame.shape[0]) == size,
                    f"源视频帧 {frame_index} 的尺寸或通道与记录不一致")

            record = None
            if (record_pos < len(records) and
                    records[record_pos]["frame_index"] == frame_index):
                record = records[record_pos]
                record_pos += 1
                counts[record["status"]] += 1

            canvas = draw_frame(frame, frame_index, record)
            writer.write(canvas)
            # 保存有记录的帧，另保留第一张未处理帧，便于对照。
            if record is not None or not unrecorded_preview:
                picture_path = output_dir / f"frame_{frame_index:06d}.png"
                require(cv2.imwrite(str(picture_path), canvas),
                        f"保存预览图片失败：{picture_path}")
                snapshots += 1
                if record is None:
                    unrecorded_preview = True
            if record is not None:
                print(
                    f"匹配帧 {frame_index} ← 请求 {record['request_id']}，"
                    f"状态 {record['status']}"
                )

        require(record_pos == len(records), "仍有记录没有匹配源视频帧")
        writer.release()
        writer = None
        print(
            f"写入流程结束：提交视频帧 {last_frame + 1}，"
            f"匹配记录 {record_pos}，无记录帧 {last_frame + 1 - record_pos}，"
            f"预览图片 {snapshots}"
        )
        print(
            f"记录状态：成功 {counts['ok']}，"
            f"服务端错误 {counts['server_error']}，"
            f"客户端错误 {counts['client_error']}"
        )
        print(f"回放文件：{video_path}")
        print("编码文件是否可完整读取，请按讲义做后续解码观察")
        return 0
    except BaseException:
        if created_output:
            print(f"回放未完成，已生成的部分文件保留在：{output_dir}",
                  file=sys.stderr)
        raise
    finally:
        try:
            if writer is not None:
                writer.release()
        finally:
            capture.release()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("用户中断回放", file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, cv2.error) as error:
        print(f"离线回放失败：{error}", file=sys.stderr)
        sys.exit(1)
