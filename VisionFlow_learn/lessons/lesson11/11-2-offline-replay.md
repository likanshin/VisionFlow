# 11-2：读取记录、匹配原帧与离线结果回放

熟悉程度已确认：你了解概念，但没有完整写过。本课重点是把已有概念接成一个可检查的程序，不重复 11-1 的 JPEG 请求和 JSONL 入门。

11-1 已由你完成五帧请求与结果记录；集中问题由老师解释，你随后要求“下一环节”。本课直接使用已有素材和 JSONL，不需要重跑视频客户端，也不需要启动 C++ 服务端。

主要代码由你亲手写，运行和验证也由你完成。老师只维护本讲义、README 和 HANDOFF；本课不修改 CMake、现有客户端或现有结果文件。

## 1. “离线回放”具体读取什么

现有输入是：

- 记录：`/home/rikanshin/VisionFlow/VisionFlow_learn/outputs/video_run_01.jsonl`
- 原视频：由每条记录的 input_video 指向 `/home/rikanshin/VisionFlow/VisionFlow_learn/samples/lesson11_demo.avi`

原视频给我们像素，JSONL 给我们“哪一帧、对应哪个请求、检测到了哪些线段”。回放程序负责把二者重新对应起来：

~~~text
JSONL → 校验 → 有序记录列表 ───────────────┐
                                         ↓ 按 frame_index 匹配
源视频 → 顺序解码 → 当前帧 → 复制画布 → 画线／标注
                                         ↓
                           replay.avi + 若干 PNG 预览
~~~

它不连接服务端，不重新运行 Canny/Hough，也不重新提交 JPEG 请求。如果服务端已经停止，本课仍然应该正常工作。

这里重现的是“曾经记录的检测结果”；算法升级后重新检测同一视频是另一件事。

## 2. 先确定输出的时间与帧范围

你的记录中，frame_index 是 0、3、6、9、12，原视频标称 10 FPS。

本课选择输出源视频从 0 到最后一条记录所在帧的完整前缀：

~~~text
输出下标：0 1 2 3 4 5 6 7 8 9 10 11 12
有记录：  ✓     ✓     ✓     ✓       ✓
~~~

因此输出视频有 13 帧、匹配五条记录、八帧没有记录；默认输出 10 FPS，名义播放时长是 13/10=1.3 秒。

这不等于把整个 20 帧素材都回放，也不等于把五帧结果凑成五帧视频。最后一条记录的估计时刻 1.2 秒是那一帧开始的位置；最后一帧还占据一个输出帧间隔，所以输出时长约 1.3 秒。

### 2.1 没有记录的帧怎么画

- 成功记录：只在对应帧画该记录的绿色线段。
- 错误记录：显示红色状态文字，不画成功检测线段。
- 没有记录：显示灰色 unrecorded 标记，不画任何检测线段。
- 成功记录的 lines 为空：仍然是成功，只是没有线段可画。

尤其不能让帧 1、2 沿用帧 0 的检测线。那样会把没有处理过的帧表现成已有检测结果。

### 2.2 输出 FPS 如何选

默认从实际打开的源视频读取标称 FPS；值无效或超出本课允许范围就报错，请你用 --fps 显式指定。不偷偷设成 30，也不把 elapsed_ms 倒数当帧率。

--fps 会改变播放速度。例如同样 13 帧设置 5 FPS，名义时长变为 2.6 秒。它不改变帧匹配，不会让算法重新运行。

本课写固定帧率 AVI。源视频若为可变帧率，这不能精确恢复原始时间轴；JSONL 的 estimated_video_time_ms 也只是一种估算。本课用已知 10 FPS 的素材观察，不宣称实现通用精确播放器。

## 3. 先校验记录，再创建输出

“json.loads 能成功”只说明文本符合 JSON 语法，不代表内容能安全、正确地拿来画线。[Python JSON 文档](https://docs.python.org/3.11/library/json.html)

本课检查直接用于回放的结构：

| 检查 | 避免的问题 |
| --- | --- |
| 顶层对象、schema_version=1 | 把别的日志格式误当成当前记录 |
| 非负整数 frame_index，正整数 request_id | 用错误类型或 bool 当编号 |
| 同一源路径、同一尺寸 | 混合不同视频或不同分辨率 |
| 帧下标和请求编号严格递增 | 重复、乱序导致记录错配 |
| 内外层编号、成功尺寸和状态一致 | 把另一请求的响应画到当前帧 |
| 每条线段四个整数，端点在图像范围内 | 坐标字段缺失、越界或类型错误 |
| server_error/client_error 的结构对应 | 错误记录被当成成功结果使用 |

我们不自动排序：排序会掩盖原日志的乱序或重复问题。本课只接收 11-1 顺序客户端产生的记录；如果将来实现并发和乱序响应，再单独定义相应的回放规则。

也不要求请求编号等于帧下标。检查严格递增允许编号有间隔，但同一帧不允许出现两条冲突记录。

### 3.1 完整尾行与损坏尾行

逐行 UTF-8 解码，再 json.loads。空行、损坏 JSON、半截尾行都明确报出物理行号，不默默跳过。

最后一行若没有换行但 JSON 本身完整，可以接受；缺少换行不自动等于数据损坏。

readline 设置字节上限，防止一个异常超长行无限增长。NaN/Infinity 拒绝接受。教学上限为：日志总字节 8 MiB、每行 128 KiB、最多 10000 条记录、输出范围最多 10000 帧、单边尺寸不超过 4096 像素、每条成功记录最多 256 条线段。这些是本工具的范围选择，不是 JSONL 标准。

### 3.2 两个仍然不能证明的事实

源路径相同、尺寸相同，不证明视频内容完全没有被替换。本课日志没有素材内容哈希，必须保留原来的源视频，不能换成同名文件还期待正确对应。

另外，服务器处理的是源帧重新编码成 JPEG 后再解码的像素，回放底图是原视频解码帧；二者可能有轻微像素差异。11-1 没有裁剪或缩放，因此坐标空间仍一致，本课按这个前提叠加原来记录的线段。

## 4. 怎样找到记录对应的原视频帧

本课不为每条记录都调用一次 CAP_PROP_POS_FRAMES 跳转，而是从头顺序 read。这样程序自己数出 0、1、2……，与 11-1 的解码序号保持一致。[VideoCapture 读取接口](https://docs.opencv.org/4.11.0/d8/dfe/classcv_1_1VideoCapture.html)

维护两个位置：

- frame_index：当前解码的视频帧下标，每读一帧前进一次。
- record_pos：下一条待匹配记录在列表中的位置，只有匹配成功才加一。

~~~text
当前帧 0，下一条记录的帧下标 0 → 匹配，record_pos 前进
当前帧 1，下一条记录的帧下标 3 → 不匹配，记录不动
当前帧 2，下一条记录的帧下标 3 → 不匹配，记录不动
当前帧 3，下一条记录的帧下标 3 → 匹配，record_pos 前进
~~~

不使用 request_id 找原视频帧，也不使用“第几行”当帧下标。

只读取到最后记录的帧。到那之前任何一次 read 失败都明确报错，因为仍有必须匹配的帧没有读到；不能把这类提前失败当作正常回放完成。

## 5. 画布、坐标和颜色

绘制使用：

~~~python
canvas = frame.copy()
cv2.line(canvas, (x1, y1), (x2, y2), (0, 220, 0), 2, cv2.LINE_AA)
~~~

- copy 创建独立画布，保留当前原始 frame 的像素。
- 坐标顺序是 (x, y)，x 向右、y 向下。
- 颜色按 BGR，绿色用 (0, 220, 0)，红色用 (0, 0, 255)。
- 2 是线宽。
- LINE_AA 使用抗锯齿绘制。[OpenCV 绘图参数](https://docs.opencv.org/4.11.0/d6/d6e/group__imgproc__draw.html)

每轮重新建立画布，不保留上一帧的 canvas，所以无记录帧不会残留上一帧的线段。

视频顶部增加黑色标注带，文字使用 JSON 字段名，如 frame_index、request_id、status。OpenCV 内置简易字体不适合直接显示中文，所以视频内用 ASCII 字段名；控制台说明仍是中文。unrecorded 只表示回放帧没有记录，不会被写回原 JSONL。

顶部标注带会遮盖原画面顶部约 31 行像素；用于教学定位帧，原视频文件不会被改动。

## 6. 输出目录和资源边界

你指定一个不存在的新目录，例如 outputs/replay_01，程序在其中写入：

~~~text
replay.avi
frame_000000.png
frame_000001.png
frame_000003.png
frame_000006.png
frame_000009.png
frame_000012.png
~~~

每个有记录的帧保存一个 PNG，另保存第一张无记录帧作对照。因此本次是五张记录预览加一张无记录预览，共六张。

新建目录使用 exist_ok=False，目录已存在就拒绝，避免覆盖旧回放。中途失败会保留部分输出并提示位置，不自动清空目录；重试时换一个新目录名。

VideoWriter 采用 MJPG/AVI，使用统一的尺寸和帧率。宽高必须是偶数，避免某些编码后端默默截去奇数边缘；本课不自动缩放或裁剪，因为那还需要同步变换线段坐标。[VideoWriter 参数与后端限制](https://docs.opencv.org/4.11.0/dd/d9e/classcv_1_1VideoWriter.html)

write() 在 Python 中不返回逐帧成功标志，所以“程序提交了 13 帧”不等于“已独立证实产物能解码出 13 帧”。实践会安排你重新打开输出文件读取，核对实际帧数。

capture 和 writer 都在 finally 中释放。发生错误时也让容器有机会收尾，但保留下来的半成品不当作成功产物。

## 7. 由你新建主要入口

文件：

`/home/rikanshin/VisionFlow/VisionFlow_learn/python/video_replay.py`

把下一节完整代码写入新文件。不修改 video_client.py，不向原 JSONL 追加内容，不改变原视频。

这一入口不导入 socket 或客户端模块，不需要服务端在线，也不需要 CMake 构建。

## 8. video_replay.py 完整代码

~~~python
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
~~~

## 9. 对照代码看四个边界

### 9.1 load_records 的输出

load_records 返回三个值：

~~~python
records, source_path, size = load_records(records_path)
~~~

records 是已经校验并保持原顺序的字典列表；source_path 是唯一源视频绝对路径；size 是 (width, height)。

先验证所有记录，再打开 VideoWriter。因此如果 JSONL 第五行损坏，不会先生成四帧视频再假装回放成功。

本课采用受 8 MiB 等上限约束的完整记录列表，便于先验证再输出；没有把视频的全部图像读进列表。后续若处理超大日志，可以另行设计流式索引，不把这份教学实现说成无限规模方案。

### 9.2 校验只覆盖我们使用的契约

绘图必须使用 frame_index、尺寸、status、request_id 和 lines，因此这些字段要认真核对。成功响应中其他字段，例如 edge_pixels、processor，本课不用于绘图定位，没有把所有业务字段都强制检查一遍。

source_fps 和 estimated_video_time_ms 也不决定本课的帧匹配；程序按顺序解码下标匹配，输出帧率取实际源视频或显式 --fps。

同路径和同尺寸仍不足以证明素材未被替换。不要把这些检查宣传成内容身份校验或通用恶意文件防护。

### 9.3 一张输出帧怎样形成

以 frame_index=3 为例：

1. 从源视频顺序解码到第 3 帧。
2. 下一条记录的 frame_index 也是 3，取出请求 2 的记录。
3. copy 当前帧作为画布。
4. 使用 result.lines 里的线段画绿色覆盖，并标注请求编号。
5. writer.write 提交这一帧；同时保存 frame_000003.png。
6. 记录指针只前进一项。

接着读第 4 帧时，下一条记录的下标是 6。此时传给 draw_frame 的 record 为 None：没有画线，也不会使用刚才请求 2 的结果。

### 9.4 程序退出码和历史检测状态分开

本课退出码：

| 退出码 | 含义 |
| --- | --- |
| 0 | 回放流程执行完成，记录均已匹配并提交输出；编码产物另按实践核对 |
| 1 | 记录、输入、绘制或输出发生错误 |
| 2 | 命令行参数不正确 |
| 130 | 用户 Ctrl+C 中断 |

假如日志中有一条合法 server_error，回放把该帧标成红色并计数，这本身不使回放命令返回 1。原来检测失败与本次成功展示该失败记录不是同一件事。

反过来，如果 lines 字段损坏或目标帧根本读不到，即便原日志声称 status=ok，回放也要明确失败。

## 10. 由你执行的实践

### 10.1 使用已有输入，不重新请求服务端

保留现有两个输入：

~~~text
outputs/video_run_01.jsonl
samples/lesson11_demo.avi
~~~

本课不需要启动服务端；若旧服务仍在运行，可以按已学的方式停止。不要为了回放重新运行 video_client.py 覆盖或替换这份记录。

现有 Python 环境已成功运行过视频客户端，本课继续使用它，不再要求重装依赖。

### 10.2 创建入口后运行

在 `/home/rikanshin/VisionFlow/VisionFlow_learn/python/video_replay.py` 写入第 8 节完整代码，然后执行：

~~~bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
python3 python/video_replay.py \
  --records outputs/video_run_01.jsonl \
  --output-dir outputs/replay_01
echo $?
~~~

如果目录已存在，换成 replay_02；下面所有检查路径也一起调整，不覆盖旧结果。

预期日志的核心内容：

~~~text
记录数：5，回放帧范围：0～12
输出尺寸：640×360，输出 FPS：10.0
离线回放：不连接服务端，不重新执行检测
匹配帧 0 ← 请求 1，状态 ok
匹配帧 3 ← 请求 2，状态 ok
匹配帧 6 ← 请求 3，状态 ok
匹配帧 9 ← 请求 4，状态 ok
匹配帧 12 ← 请求 5，状态 ok
写入流程结束：提交视频帧 13，匹配记录 5，无记录帧 8，预览图片 6
记录状态：成功 5，服务端错误 0，客户端错误 0
~~~

紧接着的退出码预期为 0。这些是预期输出，没有由老师代跑。

### 10.3 重新解码输出，检查实际产物

运行下面代码。它读取的是刚写出的 replay.avi，不是原素材；如果换了输出目录，同步修改 path。

~~~bash
python3 - <<'PY'
from pathlib import Path
import cv2
import math

path = Path("outputs/replay_01/replay.avi")
capture = cv2.VideoCapture(str(path))
try:
    if not capture.isOpened():
        raise RuntimeError(f"无法打开回放视频：{path}")
    fps = capture.get(cv2.CAP_PROP_FPS)
    count = 0
    sizes = set()
    while True:
        readable, frame = capture.read()
        if not readable:
            break
        if frame is None or frame.size == 0:
            raise RuntimeError("回放中出现空图像")
        count += 1
        sizes.add((frame.shape[1], frame.shape[0]))
    print(f"实际解码帧数：{count}")
    print(f"实际读到的尺寸：{sorted(sizes)}")
    print(f"后端报告 FPS：{fps}")
    if count != 13 or sizes != {(640, 360)}:
        raise RuntimeError("与本次五条记录对应的 13 帧、640×360 不一致")
    if not math.isfinite(fps) or abs(fps - 10.0) > 0.01:
        raise RuntimeError("与本次预期输出 FPS=10 不一致")
    print("本次回放解码观察符合预期")
finally:
    capture.release()
PY
echo $?
~~~

预期实际解码 13 帧、尺寸只出现 (640, 360)、FPS 约 10、退出码 0。这里用实际循环读取计数，不仅依赖 CAP_PROP_FRAME_COUNT 的元数据。

这个检查针对本次受控产物，不宣称能够检测所有视频损坏，也不验证线段画到哪里；位置由下一项观察。

### 10.4 查看两张对照预览

用你方便的图片查看器打开：

- `/home/rikanshin/VisionFlow/VisionFlow_learn/outputs/replay_01/frame_000000.png`
- `/home/rikanshin/VisionFlow/VisionFlow_learn/outputs/replay_01/frame_000001.png`

第一张应显示 request_id=1、status=ok，以及三条绿色检测线段；第二张应显示 request_id=-、status=unrecorded，没有新增的绿色检测线段。道路画面本身已有的白线当然还在，不把原图中的道路标线误认为程序绘制结果。

还可以查看 frame_000003.png，确认它显示 request_id=2，对应 JSONL 的第二条结果。本次第 0 帧和后续帧的检测坐标不完全相同，程序必须使用各自对应记录。

PNG 预览是编码前的画布；AVI 经 MJPG 有损编码后，颜色边缘可能略有差异，不能要求两个文件的像素逐字节相同。

如果 WSL 没有图形显示环境，直接用宿主系统打开这些图片或视频即可。本课没有 cv2.imshow、waitKey，也不要求为了查看产物安装 GUI 依赖。

### 10.5 反馈什么

请反馈：

- 回放程序的匹配与汇总日志、紧接着的退出码。
- 输出视频重新解码的帧数、尺寸、FPS 和退出码。
- 两张对照预览中，记录帧有绿色线段、无记录帧不继承绿色线段的观察。

有报错就贴完整报错。不要为本课重跑服务端、视频请求或模块 10 的停止实验。

重复目录、损坏尾行、乱序记录和提前读帧失败都有明确处理分支，但本轮不要求你故意改坏原 JSONL。尚未实际观察的错误分支不记作已经验证。

## 11. 常见定位入口

| 现象 | 先检查 |
| --- | --- |
| 源视频不存在 | JSONL 的 input_video；素材是否移动或被改名 |
| 第 N 行 JSON 错误 | 该行是否完整；不要静默丢弃尾行假装成功 |
| 帧下标或编号不递增 | 是否混入另一运行记录，或者手工打乱了行顺序 |
| 坐标越界／尺寸不一致 | 是否对视频做过缩放裁剪，是否使用了另一份同名视频 |
| 输出目录已存在 | 换新目录名，不覆盖旧回放 |
| 无法创建 MJPG/AVI | VideoWriter 后端及尺寸；保留完整错误，不先替换依赖 |
| 输出宽高为奇数被拒绝 | 本课不做自动裁剪，以免坐标错位 |
| 尚未覆盖全部记录就读帧失败 | 素材过短、损坏或发生解码错误，不当作正常结束 |
| 第 1 帧也出现第 0 帧的绿色检测线 | 是否每轮 copy 当前原帧、无记录时是否清空 record |
| 视频速度不对 | 输出 FPS 是否覆盖源 FPS；不要用网络 elapsed_ms 决定播放速度 |
| 程序返回 0 但视频无法解码 | write 没有逐帧确认值，检查 release、后端与实际产物 |
| 有 server_error 记录但回放返回 0 | 可以正确回放历史错误；回放成功与检测成功分开计量 |

## 12. 集中理解检查

完成主要实践后，一次回答：

1. 为什么必须同时保留 JSONL 和原视频？有 JSONL 为什么不能凭它重新生成原图？这次“离线”体现在哪里？
2. 五条记录为什么产生 13 帧输出？帧 1、2 为什么不能沿用帧 0 的检测线？frame_index、request_id、record_pos 分别指什么？
3. 为什么 json.loads 成功后还要校验编号、尺寸、坐标和顺序？第 5 行损坏时，本实现是否已经创建回放目录？最后一行只有缺少换行但 JSON 完整，应否拒绝？
4. 为什么先 frame.copy()，为什么 VideoWriter 的尺寸顺序是 (width, height)？release 和输出后重新解码分别解决什么问题？
5. 13 帧、10 FPS 为什么约为 1.3 秒，最后记录时刻却是 1.2 秒？显式 --fps 5 会怎样？合法错误记录是否必然让回放返回 1？同路径同尺寸能否证明源视频没被替换？

## 12.1 五点直接解答（2026-10-13）

学生明确要求“帮我回答，然后进入下一环节”。以下由老师直接解释，不记作学生独立答对；结合已完成的实现、生成与实际解码观察，按要求收尾本课。两张 PNG 可视观察尚无学生反馈，保留为未提供证据，不要求重跑，也不宣称已验证画线位置。

1. **JSONL 保存结果，原视频保存像素。** 本课 JSONL 包含源路径、帧下标、请求编号、尺寸、线段坐标和状态，没有保存每帧完整图像。坐标无法还原原视频里的道路、天空等像素，所以需要原视频。回放从本地文件读记录和像素，不连接服务端、不重新编码图片提交请求、不重新执行检测；此前服务端停止也不影响离线回放。
2. **五条记录只标识五个处理过的帧，本课输出连续前缀。** 最后记录在第 12 帧，输出下标 0～12，共 13 帧。第 1、2 帧没有记录，不能把第 0 帧的线段冒充为它们的检测结果；本课明确标为 unrecorded，不做结果传播或跟踪。frame_index 是源视频从 0 起的帧位置；request_id 是该轮请求与响应的关联编号；record_pos 是 Python 记录列表里下一条待匹配记录的位置。处理第 3 帧前，下一条记录位于列表下标 1，请求编号为 2；匹配后 record_pos 增为 2，它不等于当前帧下标。
3. **语法正确不等于业务数据正确。** 合法 JSON 仍可能把编号写成字符串、把坐标写到图像外，或混入另一段视频、重复帧和乱序记录。load_records 先读取并校验整份记录，成功返回后 main 才创建输出目录，所以第 5 行损坏时本次还没有创建回放目录。若最后一行 JSON 完整，只缺结尾换行，可以接受；若对象只写了一半则报错。本课不默默跳过损坏尾行。先校验不表示后续解码或写盘不会失败，后续失败仍可能留下部分产物。
4. **copy 分开原帧与绘图画布，release 完成资源收尾，重新解码检查实际产物。** canvas = frame.copy() 后，在 canvas 上画线和文字不会修改 frame 的像素；若后续不再使用原帧，复制并非所有绘图程序的硬要求，但这里让数据角色清楚。避免继承上一帧结果，还依赖每轮读取新帧并把 record 重置为 None，不能只归功于 copy。数组 frame.shape 的顺序是 (height, width, channels)，VideoWriter 的尺寸参数规定为 (width, height)，调用时必须换成接口要求的顺序。writer.release() 让编码器完成收尾并关闭输出，capture.release() 释放读取资源；这不能替代检查文件能否实际读回。你后续逐帧解码确认了 13 帧、640×360、10 FPS，但尚未据此确认绿色线段的位置正确。
5. **时间位置、播放时长、回放成败各有含义。** 第 12 帧的起始时刻为 12/10=1.2 秒，13 帧以 10 FPS 播放的名义时长为 13/10=1.3 秒；最后一帧也占一个帧间隔。显式 --fps 5 保留相同帧序列和匹配关系，名义时长变为 2.6 秒。合法 server_error/client_error 记录可以被正确回放为红色状态文字；只要回放自身全部步骤成功，本课程序仍返回 0。该返回码不声称历史检测都成功。同路径、同尺寸只满足基本一致性约束，无法证明文件内容没有被同名同尺寸的新视频替换；本课没有保存并校验源内容哈希。

## 13. 当前状态与接续边界

2026-10-13（11-2 按学生要求收尾）：学生明确要求“帮我回答，然后进入下一环节”。老师已直接解释五点，完整解答见 11-2 讲义第 12.1 节；据此前实现、回放生成与实际解码观察，按本次要求收尾并进入 11-3，不记作学生独立答对，也不要求重答或额外确认。两张 PNG 的可视观察仍无学生反馈，不倒填为已验证画线位置，不要求补跑。老师未修改源码或运行程序。

当前进入 [11-3 端到端集成验证](11-3-subprocess-integration.md)，熟悉程度已确认为“完全不熟悉，需要从头讲”。下方保留历史记录，以本段最新状态为准。

2026-10-13（11-2 实现、回放生成与实际解码观察通过）：学生已创建 python/video_replay.py，老师只读核对与讲义完整示例一致（忽略换行格式及末尾空白），未修改源码。学生使用 outputs/video_run_01.jsonl 生成 outputs/replay_01/replay.avi；帧 0/3/6/9/12 对应请求 1～5，全部 ok，日志汇总提交视频帧 13、匹配记录 5、无记录帧 8、预览图片 6。学生随后重新打开该输出视频逐帧解码，实际读取 13 帧，尺寸集合仅 (640, 360)，后端报告 FPS 10.0，检查提示符合预期，紧接着 echo $? 为 0。这个退出码属于后续解码检查，不倒填为回放生成命令的退出码，也不要求因此重跑。两张 PNG 的画线差别尚未收到学生观察反馈，不能凭生成与解码日志宣称可视观察通过；下一步查看 frame_000000.png 与 frame_000001.png，并完成第 12 节五道集中理解检查。未验证损坏记录、重复目录、错误响应等异常分支，不强制补跑。11-2 尚未全部完成，不提前进入下一课。老师仅只读核对并维护文档，没有运行、解码图片或视频、配置、构建或测试。

以下为此前准备记录，以本段最新反馈为准。

2026-10-13：11-1 的主要实践通过，老师按学生要求解释五点后，学生要求“下一环节”，据此收尾 11-1，不记为学生独立答对原题。11-2 熟悉程度已确认为“了解概念，但没完整写过”，不再询问。

python/video_replay.py 已由学生亲手新建并运行，回放生成及实际解码观察通过；集中五点已由老师直接解释，并按学生要求进入 11-3。两张预览的可视观察未提供，不宣称通过，也不要求补跑。老师仅维护教学资料，未写入主要源码、生成图片或视频、配置、构建、运行或测试；CMake 和既有源码、JSONL、素材保持原样。

本课输出固定帧率、无音频的教学回放，范围到最后记录帧为止。不重新检测、不填补未处理帧结果、不验证源视频内容哈希、不支持任意乱序并发日志或精确可变帧率时间轴。完成后再接续模块 11 的完整验证与性能观察，不提前实现后续内容。

接口细节已对照链接的 OpenCV/Python 官方文档；状态策略和观察参数为本项目教学选择，实际运行证据以学生反馈为准。
