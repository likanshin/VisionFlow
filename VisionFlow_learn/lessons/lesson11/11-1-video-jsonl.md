# 11-1：从单张图片到视频抽帧、逐帧请求与 JSONL 记录

熟悉程度：你已明确表示“完全不熟悉，需要从头讲”。模块 1～10 已按教学记录收尾，本课从视频输入开始，不重复讲解 TCP 和线程池。

本课完成一个闭环：从本地视频选取若干帧，把每帧变成现有服务端能接收的 JPEG 请求，再把对应的结果逐行存到文件。后续 11-2 才使用这些记录做离线回放。

主要代码由你亲手编写；老师只维护本讲义、README、HANDOFF 等辅助资料，不替你配置、构建、运行或测试。

## 1. 视频、帧、帧率分别是什么

你可以先把视频理解为带有时间信息的一系列图像：

~~~text
帧 0 → 帧 1 → 帧 2 → 帧 3 → …
~~~

视频文件通常经过压缩，不能靠“按固定字节数切文件”得到一张帧图。解码器会把文件中的编码数据恢复成像素图像。

本课用 OpenCV 的 VideoCapture 顺序解码。Python 中读出的 frame 通常是 NumPy 数组，与之前 C++ 的 cv::Mat 一样，用来承载解码后的像素；对本课普通彩色视频，形状是 (height, width, channels)，通道顺序是 BGR。不需要你手工实现视频解码器。[VideoCapture 文档](https://docs.opencv.org/4.x/d8/dfe/classcv_1_1VideoCapture.html)

FPS 是 frames per second，即每秒帧数。假设固定帧率是 10：

| 帧下标 | 估计对应的视频时刻 |
| --- | --- |
| 0 | 0 毫秒 |
| 1 | 100 毫秒 |
| 3 | 300 毫秒 |
| 12 | 1200 毫秒 |

估算公式是 frame_index × 1000 / FPS。这里的 frame_index 从 0 开始。

这不是当前电脑的墙上时间，也不是网络请求耗时。对于可变帧率、缺失或不准确的元数据，这个公式不能提供精确的呈现时间。因此字段明确叫 estimated_video_time_ms；FPS 无效时记录 null，不伪造 0，也不假定 30 FPS。

## 2. 为什么服务器不需要重新写

现有服务端的输入仍是一张 JPEG 图片，对每个连接接收一个请求、返回一个结果，然后关闭该连接。

视频客户端负责重复做同一件事：

~~~text
本地视频文件
   ↓ VideoCapture.read()
一帧像素图像
   ↓ cv2.imencode(".jpg", frame, ...)
JPEG 字节
   ↓ VFLW 请求，一条新的 TCP 连接
现有 C++ 服务端
   ↓ VFLW 响应
Python 字典
   ↓ 补充帧下标、请求编号等信息
JSONL 中的一行记录
~~~

服务端只需知道“这次请求的 JPEG 和 request_id”。视频路径、帧下标和估计视频时刻由客户端记录，暂时不修改网络协议。

本课选择顺序请求：上一帧收到响应并记录以后，才发送下一帧。客户端在任意时刻最多等待一个图片请求，便于学习编号和记录对应关系；这不是并发压测，也不承诺实时播放速度。

由于服务器响应后关闭连接，不能建立一个连接就不断往里面塞新帧。本课每个请求各自建立连接，保留现有服务端的连接约定。

## 3. 抽帧间隔和处理上限不是同一个参数

--stride 3 表示选择下标能被 3 整除的帧：

~~~text
读取：0 1 2 3 4 5 6 7 8 9 10 11 12 …
选择：0     3     6     9       12 …
~~~

本课仍顺序解码每帧，未选中的帧不编码 JPEG、不请求服务端。这里先实现清楚的顺序逻辑，不引入随机跳转或关键帧定位。

--max-frames 5 表示最多尝试处理五个选中帧，不是视频最多读取五帧，也不是必须成功五次后才停止。

如果视频足够长，stride=3、max-frames=5 的对应关系为：

| 选中次序 | frame_index | request_id |
| --- | --- | --- |
| 1 | 0 | 1 |
| 2 | 3 | 2 |
| 3 | 6 | 3 |
| 4 | 9 | 4 |
| 5 | 12 | 5 |

frame_index 标记视频位置；request_id 标记本次运行的请求。它们不能混用。本课 request_id 从 1 递增，只保证在一个运行记录文件内不重复，不是跨客户端、跨运行的全局编号。

即使两帧像素完全相同，也会产生不同的请求编号。

## 4. 从像素图像变成 JPEG 字节

~~~python
encoded_ok, encoded = cv2.imencode(
    ".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 90]
)
jpeg = encoded.tobytes()
~~~

逐个看参数和返回值：

- ".jpg" 选择 JPEG 编码器，不是输出文件路径。
- frame 是解码后的像素数组。
- 参数列表按“键、值”成对填写，JPEG 质量本课固定为 90。
- encoded_ok 表示编码是否成功，失败时不能继续取结果发送。
- encoded 是编码后的字节数组，不再是原来的二维／三维像素图。
- tobytes() 得到 Python bytes，交给 socket.sendall。

这里直接在内存编码，不需要先把每帧保存成 frame_001.jpg，再从磁盘读回来。[OpenCV imencode 文档](https://docs.opencv.org/4.x/d4/da8/group__imgcodecs.html)

为什么不能直接 frame.tobytes() 发送？它得到的是原始像素字节，缺少 JPEG 编码结构；当前服务器使用图片解码流程，期待 JPEG 内容，不能把裸像素当 JPEG。

本课仍检查编码后大小不超过现有 16 MiB 上限。这个限制针对压缩请求内容，不是对解码后像素内存的完整预算。

素材会经过视频编码、视频解码和 JPEG 编码。JPEG/MJPG 有损，因此不要要求最后的边缘像素数始终等于旧单图片实验的 2095；本课关注编号、帧位置、尺寸及响应记录正确对应。

## 5. 为什么保存 JSONL

JSON 是一份结构化数据。你已经用它保存过单次处理结果。

JSONL（JSON Lines）把多个独立 JSON 值放在文本文件中，一行一个。我们规定每行都是一个对象，文件使用 UTF-8，并在每条记录后写换行。[JSON Lines 格式说明](https://jsonlines.org/)

~~~json
{"request_id":1,"frame_index":0,"status":"ok"}
{"request_id":2,"frame_index":3,"status":"ok"}
~~~

这两行是两个 JSON 对象，不是包在 [] 里的一个大数组。上面的例子只展示格式，完整记录还包含更多字段。

保存方式是：

~~~python
output.write(json.dumps(record, ensure_ascii=False, allow_nan=False) + "\n")
output.flush()
~~~

- dumps 把一条 Python 字典转换成文本。
- ensure_ascii=False 保留中文字符。
- 不使用 indent，让一条记录占一行；字符串内的换行会被编码转义。
- allow_nan=False 拒绝 NaN/Infinity 等非标准 JSON 数值。
- flush 把 Python 缓冲交给下面的文件层，不代表断电时必然持久保存，也不是 fsync。[Python JSON 编解码](https://docs.python.org/3.11/library/json.html)

程序逐条写，不把整个视频的结果攒到大列表。已写好的完整行可独立读取；突然中断仍可能留下不完整的最后一行，后续回放需要检查，不能假定整个文件总是完整。

结果文件使用 x 模式新建：存在就报错。每次运行换一个文件名，不悄悄覆盖以前的学习记录，也不自动追加进上一次运行。

## 6. 本课记录的字段

| 字段 | 含义 |
| --- | --- |
| schema_version | 本地日志结构版本，固定为 1；不是 VFLW 协议版本字段 |
| input_video | 本次输入视频的绝对路径，后续回放可定位素材 |
| source_fps | 后端报告的标称 FPS；无有效值时为 null |
| frame_index | 解码序列中从 0 开始的帧下标 |
| estimated_video_time_ms | 根据标称 FPS 估算的视频时刻；未知时 null |
| request_id | 本次运行内从 1 递增的请求编号 |
| width / height | 本次选中帧的尺寸，没有在客户端缩放 |
| jpeg_bytes | 编码后的 JPEG 字节数；尚未成功编码时 null |
| elapsed_ms | 从编码前到响应校验结束／错误捕获的耗时 |
| status | 本地记录状态：ok、server_error 或 client_error |
| result | 完整服务端结果字典；没有取得有效响应时 null |
| error | 客户端错误类型和消息；没有客户端错误时 null |

elapsed_ms 包含 JPEG 编码、连接、收发、服务端等待／处理和响应校验；不包含前面的解码及后面的记录写盘，所以不能把它当作“算法纯耗时”或“视频播放时刻”。

三个状态怎么处理：

| 情况 | 本地 status | 后续行为 |
| --- | --- | --- |
| 合法成功响应 | ok | 写一行，继续下一个选中帧 |
| 合法错误响应，例如 busy | server_error | 保存完整错误响应，继续下一帧，不重试本帧 |
| 编码失败、连接失败、超时、响应格式错误 | client_error | 保存错误后停止本次运行，不自动重试 |

现有服务端 busy 响应的 JSON 是 status="error"、code="busy"。本地记录外层用 server_error 分类，内层 result 保留服务端原文，两层状态不是同一个字段语义。

只要发生已记录的服务端或客户端错误，本次命令最终返回 1；正常处理了至少一帧且没有这些错误才返回 0。参数错误返回 2，用户 Ctrl+C 返回 130。

## 7. 需要你新建的文件

学习工程：`/home/rikanshin/VisionFlow/VisionFlow_learn`。

只新增主要入口：

`/home/rikanshin/VisionFlow/VisionFlow_learn/python/video_client.py`

下面给出完整代码。保留原来的 `/home/rikanshin/VisionFlow/VisionFlow_learn/python/vision_client.py`；新文件导入它的 HEADER、大小上限和 recv_exact，不复制另一套协议常量，也不调用它固定 request_id=42 的 main。

按照 `python3 python/video_client.py ...` 运行时，Python 会从脚本目录查找同目录模块。旧文件的 main 有 __name__ 守卫，因此 import vision_client 不会发出一张图片请求。

这是 Python 入口，本课不新增 CMake 目标，也不修改现有 C++ 服务端或监管器。

## 8. video_client.py 完整代码

~~~python
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
~~~

## 9. 对照完整代码理解 Python 的控制流

### 9.1 参数进入程序后是什么

parse_args 返回一个对象，args.input 和 args.output 是 Path，args.stride 和 args.max_frames 是正整数。

LessonParser 只调整参数错误的提示入口，主要解析仍由标准库 argparse 处理。--help 显示说明后退出，不打开视频，不连接服务端。

resolve 把相对路径按当前工作目录解释为绝对路径。视频本身必须是现有本地文件；本课不把摄像头编号、网络视频 URL 混进同一个入口。

### 9.2 request_image 每次只完成一个往返

它消耗四个输入：JPEG bytes、请求编号、宽度、高度，返回一个已做基本一致性检查的 Python 字典。

本课沿用既有 VFLW 24 字节头，并检查：

1. 魔数、版本、flags 和响应类型。
2. 二进制头中的 response_id 等于本次 request_id。
3. 响应体长度为正且不超过现有上限。
4. JSON 顶层是对象、request_id 是真正的整数且编号相同。
5. JSON status 与响应类型对应，成功结果的尺寸与发送帧对应。
6. 错误响应有字符串 code；非标准 NaN/Infinity 数值被拒绝。

这里 type(value) is int 是特意的：Python 的 bool 属于 int 的子类，不能让 true 被当成请求编号 1。后续回放还需要检查线段等业务字段，本课不把“基本一致性检查”说成所有响应字段均已完整验证。

SOCKET_TIMEOUT=5.0 是 socket 操作超时设置。recv_exact 可能多次 recv，所以它不等于整段视频或整次完整请求的严格五秒总期限。[Python socket 超时](https://docs.python.org/3.11/library/socket.html)

sendall/recv_exact 不会自动重试整项业务。网络中断时，客户端无法仅凭没收到响应就断言服务端没处理过；本课记录 client_error 后停止，避免自动重发掩盖这一点。

### 9.3 decoded 与 processed 为什么分开

decoded 每成功解码一帧就加一。先用旧值作为 frame_index，然后递增，保证第一帧下标是 0。

processed 只在一条选中帧记录成功写出之后加一。它既决定下一个 request_id，也限制记录数量。它包含 server_error 和已记录的 client_error，不是成功计数；成功另由 successes 统计。

循环中 continue 表示这帧不选，直接读下一帧；break 表示结束整次选帧循环。

达到五个选中帧时就停，即使视频后面还有图像。本课会直接读出 0～12 共 13 帧，选择其中五帧；不会为了“读完整个视频”额外读取剩下的素材。

### 9.4 异常在哪一层处理

帧内 try 包住 JPEG 编码和请求响应。失败后先构造 error 字段，再把当前记录写到 JSONL，随后停止。这让你能查到“哪个视频帧、哪个请求失败”。

打开输入失败、视频没有任何帧、创建／写入结果文件失败等由外层报告并退出。这些情形可能来不及形成某一帧的完整记录，不能承诺所有错误都写进 JSONL。

with 负责关闭文件和网络连接。VideoCapture 则在 finally 里 release：即使中途异常，也释放视频资源。这与前面 C++ 的资源生命周期管理解决的是同一类问题，Python 写法不同。

Ctrl+C 走 KeyboardInterrupt，返回 130。此前完整写入的行会保留，当前正在处理或写入的一帧不保证存在完整行。

### 9.5 三个时间不要混淆

| 时间量 | 来源 | 用途 |
| --- | --- | --- |
| source_fps | 视频元数据 | 描述源视频标称帧率 |
| estimated_video_time_ms | frame_index / FPS | 估算该帧在源视频中的位置 |
| elapsed_ms | perf_counter 差值 | 观察本次编码到响应校验的处理耗时 |

perf_counter 用于短时间间隔，返回的绝对起点没有业务意义；只使用结束减开始。[Python 时间接口](https://docs.python.org/3.11/library/time.html)

如果服务器很快，程序可以快于视频播放速度处理；如果很慢，也可能处理一帧就花很久。本课没有按 FPS sleep，没有显示窗口，不是播放器。

### 9.6 读不到帧意味着什么

VideoCapture.read 返回失败时，可能是正常结尾，也可能是损坏、截断或解码器失败，不能只凭这个布尔值精确区分。

因此程序打印“可能到达末尾或发生解码失败”。只要已经成功完成了一些选中请求且没有记录的请求错误，它可以返回 0，但这只表示这些请求成功，不证明源视频完整无损、所有预定帧都已处理。

本课受控素材足够长，正常实践应达到五条记录；若不足五条，贴出读取情况分析，不把不够五条自动判定为完整成功。

## 10. 由你准备素材与执行

### 10.1 先确认现有 Python 环境

原项目 environment.yml 已声明 Python 3.11、numpy 1.26.4、opencv-python 4.11.0.86。文件声明不证明当前解释器实际安装情况；C++ 能链接 OpenCV 也不能证明 Python 能 import cv2。

在你已激活的 visionflow 环境中执行：

~~~bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
python3 -c "import cv2; print(cv2.__version__)"
~~~

能显示版本就继续，不因为与声明有差异就自动重装。若无法导入，贴出错误后再定位当前 Python 环境；老师不会代你安装或改环境。

### 10.2 用已有图片生成一个短视频素材

项目目前没有发现可用视频文件。本课用 samples/frame.jpg 重复写入 20 帧、标称 10 FPS 的 MJPG/AVI 素材，视频内容静止，长度约两秒。它用来验证视频读取和多次请求，不用于评价动态道路识别效果。

在学习工程目录中执行下面整段命令。它是素材准备代码，不放进 video_client.py：

~~~bash
python3 - <<'PY'
from pathlib import Path
import cv2

image_path = Path("samples/frame.jpg")
video_path = Path("samples/lesson11_demo.avi")
if video_path.exists():
    raise FileExistsError(f"素材已存在，保留原文件：{video_path}")

frame = cv2.imread(str(image_path))
if frame is None:
    raise ValueError(f"无法读取素材图片：{image_path}")
height, width = frame.shape[:2]
writer = cv2.VideoWriter(
    str(video_path),
    cv2.VideoWriter_fourcc(*"MJPG"),
    10.0,
    (width, height),
)
try:
    if not writer.isOpened():
        raise RuntimeError("无法创建 MJPG/AVI 素材，请反馈完整错误")
    for _ in range(20):
        writer.write(frame)
finally:
    writer.release()
print(f"素材写入流程结束：{video_path}；20 帧，标称 10 FPS")
print("是否能正常解码，由后续视频客户端的实际读取确认")
PY
~~~

VideoWriter 的参数依次是输出路径、编码器标识、帧率和 (width, height)。注意尺寸顺序与 frame.shape 中的 height、width 相反。MJPG 选择 Motion JPEG；是否支持取决于当前 OpenCV 后端，isOpened 失败时直接反馈，不需要你猜另一套依赖。

finally 中 release 让视频容器有机会完成收尾写入。写入函数本身不提供完整的逐帧成功证明，素材可用性仍由后续解码观察确认。[VideoWriter 文档](https://docs.opencv.org/4.11.0/dd/d9e/classcv_1_1VideoWriter.html)

素材已存在时不覆盖；如果是你此前已成功生成的同名素材，可以直接复用。若上次生成失败留下半成品，换一个素材文件名并同步调整 --input，不要求删除原文件。

### 10.3 写入主要入口

按照第 8 节完整代码创建：

`/home/rikanshin/VisionFlow/VisionFlow_learn/python/video_client.py`

不需要编译 Python，不执行 CMake。本课直接使用你已经构建运行成功的 process_supervisor 和 vision_pool_server。

### 10.4 启动现有服务端

终端 A：

~~~bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
./build/process_supervisor ./build/vision_pool_server
~~~

如果这个服务已经在运行，就复用当前实例，不再启动第二个占用 19090 的进程。等监听日志出现后再运行客户端。

本课使用默认算法、默认零观察延迟，不加 --demo-delay-ms。也不启动其他观察脚本制造过载。

### 10.5 执行一次五帧请求与记录

终端 B：

~~~bash
cd /home/rikanshin/VisionFlow/VisionFlow_learn
python3 python/video_client.py \
  --input samples/lesson11_demo.avi \
  --output outputs/video_run_01.jsonl \
  --stride 3 \
  --max-frames 5
echo $?
~~~

如果输出文件已存在，换成 video_run_02.jsonl 等新名字，不覆盖旧记录。

预期看到五条处理行：

~~~text
帧 0 → 请求 1：ok，约 … 毫秒
帧 3 → 请求 2：ok，约 … 毫秒
帧 6 → 请求 3：ok，约 … 毫秒
帧 9 → 请求 4：ok，约 … 毫秒
帧 12 → 请求 5：ok，约 … 毫秒
已达到选中帧处理上限
汇总：读取帧 13，记录 5，成功 5，服务端错误 0，客户端错误 0
~~~

紧接着的 echo $? 预期为 0。这是预期，没有老师代跑得到的结果。

服务端应收到五个编号为 1～5 的图片请求，每次连接在响应发完后关闭。连接 token 不必等于 request_id，不要求日志 token 从某个固定数字开始。

### 10.6 检查 JSONL 的对应关系

仍在终端 B 执行下面代码。如果你改了输出文件名，也修改这里的 path：

~~~bash
python3 - <<'PY'
import json
from pathlib import Path

path = Path("outputs/video_run_01.jsonl")
count = 0
with path.open(encoding="utf-8") as stream:
    for line_number, line in enumerate(stream, 1):
        item = json.loads(line)
        count += 1
        result = item["result"]
        print(
            f"第 {line_number} 行：帧={item['frame_index']}，"
            f"请求={item['request_id']}，状态={item['status']}，"
            f"估计视频时刻={item['estimated_video_time_ms']} 毫秒，"
            f"响应编号={result.get('request_id') if result else None}"
        )
print(f"总记录数：{count}")
PY
~~~

预期共五行，帧下标依次 0、3、6、9、12，请求编号依次 1～5，响应中的编号与之相同，状态都是 ok。受控素材 FPS 为 10 时，估计视频时刻为 0、300、600、900、1200 毫秒。

每行单独 json.loads；不要对整个 JSONL 文件调用一次 json.load，它不是一个顶层 JSON 数组。

检查完以后，在终端 A 用 Ctrl+C 停止监管器和服务端即可；这是使用已完成的停止功能，不再重做模块 10 的问答。

### 10.7 反馈什么

请贴出：

- 视频客户端的五条处理日志、汇总和紧接着的退出码。
- JSONL 检查输出，确认帧下标、请求编号及响应编号对应。
- 服务端对应五个请求的关键日志；有报错就保留完整报错。

不要求精确复现每帧的耗时或旧 JPEG 的边缘数。先完成这个正常闭环，再集中讨论理解；缺失视频、重复输出名、服务端断开等分支已经说明策略，没有运行证据时不记作验证通过，也不要求一次补齐全部故障实验。

## 11. 遇到问题先查什么

| 现象 | 先检查 |
| --- | --- |
| No module named cv2 | 当前激活环境和实际 python3；先贴错误，不盲目重装 |
| 无法创建／打开 AVI | 素材生成输出、VideoWriter.isOpened、文件路径和编码后端 |
| No module named vision_client | 新文件是否位于 python 目录，是否按讲义脚本路径运行 |
| 连接被拒绝 | 服务端是否已监听、是否刚被停止、地址是否仍为 127.0.0.1:19090 |
| 只写一行 client_error 就停 | 查看这一行 error.type / error.message；这符合失败即停止策略 |
| JSONL 已存在 | 换新输出文件名，不覆盖或清空旧记录 |
| 请求 1 对应帧 0，而不是帧 1 | 两种编号故意有不同起点和含义 |
| 读取 13 帧但只有 5 条记录 | stride 3 选中 0、3、6、9、12，其他帧只解码不发送 |
| JSONL 不能整体 json.load | 按行分别 json.loads |
| 边缘数量与静态 JPEG 不同 | 视频与 JPEG 重新编码会改变像素，不以固定 2095 作为验收条件 |
| 启动器运行着，但服务端已退出 | 看子进程退出日志及服务端错误，不把无启动失败报告当作就绪 |
| read 过早失败，只得到少数记录 | 可能是素材或解码问题，不能仅凭已有请求成功宣称整段视频完好 |

## 12. 集中理解检查

实践后一次回答五点，不需要中途每写几行就停下来答题：

1. VideoCapture.read 得到的是 JPEG 文件字节还是像素数组？为什么不能直接把 frame.tobytes() 交给现有服务器？
2. stride=3、max-frames=5 且视频足够长时，选中的 frame_index 和 request_id 各是什么？为什么实际解码帧数是 13 而不是 5？
3. 为什么每个选中帧建立一个新连接？顺序请求能否让这一客户端同时占满两个工作线程？服务端 busy 时，记录外层和内层分别是什么状态？
4. JSONL 与一个 JSON 数组有什么区别？为什么不用 indent、为什么逐行写？flush 是否意味着断电不丢数据？为什么输出用 x 模式？
5. estimated_video_time_ms 与 elapsed_ms 有什么区别？FPS 不可用时如何记录？如果第 3 个选中帧连接失败，日志和退出码如何变化，程序会自动重发吗？

## 12.1 五点直接解答（2026-10-13）

学生要求“直接帮我解答”，老师直接解释如下；不记作学生独立答对，也不要求重答原题。学生随后要求进入下一环节，接续状态见第 13 节。

1. VideoCapture.read() 得到解码后的像素数组，普通彩色视频通常是 BGR 排列。frame.tobytes() 只是把原始像素按内存顺序展开，不会自动形成 JPEG。当前服务端接收编码图片并调用图片解码流程，因此先 cv2.imencode(".jpg", frame, ...) 得到 JPEG 编码数组，再 encoded.tobytes() 得到请求体 bytes。
2. stride=3、max-frames=5 时，选中帧下标为 0、3、6、9、12，请求编号为 1、2、3、4、5。顺序读取必须经过第 0 到第 12 帧，共 13 帧；其中八帧解码后跳过，不编码和发送。frame_index 定位视频解码序列位置，request_id 对应本次运行内的请求与响应，跨运行不保证唯一。
3. 现有服务端一连接只处理一个图片请求，响应发完便关闭，所以每个选中帧建立新连接。这个客户端收到并记录上一帧结果后才发下一帧，因此不会同时有两项图片计算占用两个 worker；不同请求可以由不同 worker 轮流处理。合法 busy 响应在记录外层为 status="server_error"，内层 result.status="error"、result.code="busy"。保存错误后继续下一选中帧，不重试该帧；整次命令最终返回 1。
4. JSONL 是每行一个独立 JSON 值，本课每行一个对象；JSON 数组则是由 [] 和逗号组成的一份整体 JSON。逐行写不需要攒完整结果列表，也便于逐行读取。indent 会让一条对象跨多个物理行，破坏本课“一行一条记录”的约定，所以不使用。flush 只把 Python 缓冲交给底层文件层，不等于 fsync，不保证断电无损。x 模式只创建新文件，已有文件就失败，以免覆盖旧记录或把两次从 request_id=1 开始的运行混写；它不是追加模式。
5. estimated_video_time_ms = frame_index × 1000 / source_fps，估计帧在原视频时间轴中的位置；elapsed_ms 是本次 JPEG 编码、连接、收发、服务端等待／计算与响应校验的合计耗时，不包含前面的解码和后面的日志写盘，也不是纯算法耗时。FPS 无效时 source_fps 和 estimated_video_time_ms 都写 null。如果前两帧成功、第三个选中帧连接失败且结果文件写入正常，则共留下三条记录：前两条 ok，第三条 frame_index=6、request_id=3、status="client_error"、result=null，并含 error 类型和消息。记录后停止，不请求 4、5，汇总为记录 3、成功 2、客户端错误 1，返回 1，不自动重发。若连接在请求发出后中断，没收到结果也不能据此断言服务端没处理过，这也是不自动重试的原因之一。

## 13. 当前状态与接续边界

2026-10-13（11-1 收尾）：学生在老师直接解答五点后要求“下一环节”。据此前实现、五帧请求与 JSONL 主要实践，以及此次明确推进要求，收尾 11-1；不记作学生独立答对原题，不要求重答、补跑或再次确认。

当前接续 [11-2 离线结果回放](11-2-offline-replay.md)。学生已确认“了解概念，但没完整写过”；下方保留此前各阶段记录，以本段最新状态为准。

2026-10-13（11-1 五点由老师解答，理解确认待完成）：学生要求“直接帮我解答”。老师已解释像素数组与 JPEG 编码、13 帧解码与五帧抽样／两种编号、单请求连接和顺序并发限制、busy 内外层状态、JSONL 与数组／indent／flush／x 模式，以及视频估计时刻与处理耗时、无效 FPS 和第三帧连接失败的三条记录及退出码 1。完整解释见讲义第 12.1 节。不记作学生独立答对五题，不重复原题或要求重跑；当前仅待学生确认理解，再接续 11-2。本次请求只有解答，没有要求直接进入下一课，因此不自动收尾 11-1 或生成 11-2。主要运行观察仍已通过，老师未修改源码或结果，未配置、构建、运行或测试。

2026-10-13（11-1 实现与主要运行观察通过，集中理解检查待完成）：学生已创建 python/video_client.py，老师只读核对与当前讲义完整代码一致。学生以 samples/lesson11_demo.avi、stride=3、max-frames=5 运行客户端，标称 FPS 10.0；选中帧 0/3/6/9/12 分别对应请求 1/2/3/4/5，全部 ok；汇总读取 13 帧、记录 5、成功 5、服务端错误 0、客户端错误 0。显示耗时分别约 26.73、7.91、7.16、7.37、7.34 毫秒，包含编码和往返等，不当作算法纯耗时，也不据首帧较慢断言原因。学生逐行解析 JSONL，确认估计视频时刻 0/300/600/900/1200 毫秒、请求与响应编号逐条一致，总数 5。老师只读核对 outputs/video_run_01.jsonl：五条记录均 schema_version=1、640×360、result.processor=lane_lines、内外层 status=ok、error=null；首条 JPEG 12874 字节、edge_pixels=2120，其余 13042 字节、edge_pixels=2116，不要求重新编码后的边缘数仍是旧单 JPEG 的 2095。没有老师运行代码、解码素材或执行检查脚本。学生未展示 echo $?、素材生成输出、Python cv2 版本或本轮服务端日志，不倒填，也不要求为此重跑；已有客户端完整响应与落盘文件足以确认本课主要闭环。当前只接续本课五道集中理解检查，不重复熟悉程度、不提前进入 11-2。未验证错误、断连、中断等分支，不强制追加故障实验。老师仅维护辅助文档，未修改源码或结果文件，未安装依赖、配置、构建、运行或测试。

2026-10-13：学生要求老师直接解释 10-3 五点并进入下一环节，随后明确表示 11-1 完全不熟悉，需要从头讲。模块 10 按该明确请求收尾，不记录为学生独立答对 10-3 五题。

本课 Python 主要入口已由学生创建，客户端请求与 JSONL 记录观察通过；五点已由老师直接解释，随后按学生进入下一环节的要求收尾。老师只读核对源码和结果，未生成视频、安装依赖、配置、构建、运行或测试；本课没有 CMake 修改。

本课完成后接续 11-2：读取这些 JSONL 记录与对应源视频，做离线结果回放。JSONL 保存的是结果和源路径，不包含视频图像本身；保留原视频，避免换成同名但不同内容的文件。精确可变帧率时间轴、自动重试、并发请求、完整视频损坏检测和压测不在本课提前实现。

API 细节对照所链接的 OpenCV 与 Python 官方文档；参数、日志结构和教学实践是本项目的选择。所有示例输出都只是预期，实际验证以学生反馈为准。
