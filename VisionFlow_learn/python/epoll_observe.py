import json
import socket
import struct
import sys
from pathlib import Path

from vision_client import HEADER, recv_exact


def request(jpeg, request_id):
    return HEADER.pack(b"VFLW", 1, 1, request_id, len(jpeg), 0) + jpeg


def show_response(connection, request_id, label):
    header = recv_exact(connection, HEADER.size)
    magic, version, kind, response_id, length, flags = HEADER.unpack(header)
    if magic != b"VFLW" or version != 1 or flags != 0:
        raise ValueError("响应消息头格式不正确")
    if kind not in (2, 3) or response_id != request_id:
        raise ValueError("响应类型或编号不正确")
    if not 0 < length <= 64 * 1024 - HEADER.size:
        raise ValueError("响应长度超出允许范围")
    result = json.loads(recv_exact(connection, length).decode("utf-8"))
    expected_status = "ok" if kind == 2 else "error"
    if not isinstance(result, dict):
        raise ValueError("响应 JSON 必须是对象")
    if result.get("request_id") != request_id:
        raise ValueError("JSON 请求编号不一致")
    if result.get("status") != expected_status:
        raise ValueError("响应类型与 JSON 状态不一致")
    print(f"{label}收到响应：编号={response_id}，状态={result['status']}")
    if kind != 2:
        raise RuntimeError("本次图片处理失败")


def main():
    jpeg = Path("samples/frame.jpg").read_bytes()
    if not 0 < len(jpeg) <= 16 * 1024 * 1024:
        raise ValueError("图片大小必须在 1 字节到 16 MiB 之间")
    address = ("127.0.0.1", 19090)
    slow_request = request(jpeg, 101)
    with socket.create_connection(address, timeout=15) as slow:
        slow.sendall(slow_request[:10])
        print("A 连接只发送了头部的前 10 字节，暂不补齐")
        with socket.create_connection(address, timeout=15) as fast:
            fast.sendall(request(jpeg, 102))
            show_response(fast, 102, "B 连接")
        slow.sendall(slow_request[10:])
        print("A 连接继续发送剩余请求")
        show_response(slow, 101, "A 连接")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, struct.error) as error:
        print(f"观察失败：{error}", file=sys.stderr)
        sys.exit(1)
