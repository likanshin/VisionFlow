from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import ExitStack
import json
from pathlib import Path
import socket
import sys
import time

from vision_client import HEADER, MAX_PAYLOAD, MAX_RESPONSE_BODY, recv_exact


def receive_result(connection, request_id, started):
    raw = recv_exact(connection, HEADER.size)
    magic, version, kind, response_id, length, flags = HEADER.unpack(raw)
    if magic != b"VFLW" or version != 1 or flags != 0:
        raise ValueError("响应消息头格式不正确")
    if kind not in (2, 3) or response_id != request_id:
        raise ValueError("响应类型或请求编号不正确")
    if not 0 < length <= MAX_RESPONSE_BODY:
        raise ValueError("响应消息体长度超出允许范围")
    result = json.loads(recv_exact(connection, length).decode("utf-8"))
    expected = "ok" if kind == 2 else "error"
    if (not isinstance(result, dict)
            or result.get("request_id") != request_id
            or result.get("status") != expected):
        raise ValueError("响应消息头与 JSON 不一致")
    if kind == 3 and result.get("code") != "busy":
        raise ValueError(f"收到非预期的业务错误：{result}")
    return request_id, result, time.monotonic() - started


def main():
    if len(sys.argv) > 2:
        raise ValueError("用法：python3 python/pool_observe.py [图片路径]")
    path = Path(sys.argv[1]) if len(sys.argv) == 2 else Path("samples/frame.jpg")
    jpeg = path.read_bytes()
    if not 0 < len(jpeg) <= MAX_PAYLOAD:
        raise ValueError("图片文件大小必须在 1 字节到 16 MiB 之间")

    counts = {"ok": 0, "busy": 0}
    with ExitStack() as stack:
        connections = [
            stack.enter_context(socket.create_connection(
                ("127.0.0.1", 19090), timeout=40))
            for _ in range(6)
        ]
        started = time.monotonic()
        for request_id, connection in zip(range(201, 207), connections):
            header = HEADER.pack(b"VFLW", 1, 1, request_id, len(jpeg), 0)
            connection.sendall(header + jpeg)
        print("六个请求已发送，下面按客户端观察到的完成顺序显示响应")
        with ThreadPoolExecutor(max_workers=6) as executor:
            futures = [
                executor.submit(receive_result, connection, request_id, started)
                for request_id, connection in zip(range(201, 207), connections)
            ]
            for future in as_completed(futures):
                request_id, result, elapsed = future.result()
                label = "ok" if result["status"] == "ok" else "busy"
                counts[label] += 1
                print(f"请求 {request_id}：{label}，约 {elapsed:.2f} 秒")
    print(f"汇总：成功 {counts['ok']}，忙碌 {counts['busy']}")
    if counts["busy"] == 0:
        print("本轮没有触及忙碌分支；请先反馈实际输出和服务端延迟参数")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"线程池观察失败：{error}", file=sys.stderr)
        sys.exit(1)
