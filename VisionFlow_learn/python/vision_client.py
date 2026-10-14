import json
from pathlib import Path
import socket
import struct
import sys

# 消息头格式：4 字节魔数 + 2 字节版本号 + 2 字节消息类型 + 8 字节请求编号 + 4 字节消息体长度 + 4 字节标志位
HEADER = struct.Struct("!4sHHQII")

MAX_PAYLOAD = 16 * 1024 * 1024
MAX_RESPONSE_BODY = 64 * 1024 - HEADER.size


def recv_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise ConnectionError("连接已关闭，尚未收齐消息数据")
        data.extend(chunk)
    return bytes(data)


def main():
    input_path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("samples/frame.jpg")
    if len(sys.argv) > 2:
        raise ValueError("用法：python3 python/vision_client.py [图片路径]")
    jpeg = input_path.read_bytes()
    if not 0 < len(jpeg) <= MAX_PAYLOAD:
        raise ValueError("图片文件大小必须在 1 字节到 16 MiB 之间")

    request_id = 42
    request = HEADER.pack(b"VFLW", 1, 1, request_id, len(jpeg), 0) + jpeg
    with socket.create_connection(("127.0.0.1", 19090)) as connection:
        connection.sendall(request)
        print(f"图片请求已发送，请求编号：{request_id}")
        header = recv_exact(connection, HEADER.size)
        magic, version, kind, response_id, length, flags = HEADER.unpack(header)
        if magic != b"VFLW" or version != 1 or flags != 0:
            raise ValueError("响应消息头格式不正确")
        if kind not in (2, 3):
            raise ValueError("响应消息类型必须是结果或错误")
        if response_id != request_id:
            raise ValueError("响应编号与请求编号不一致")
        if not 0 < length <= MAX_RESPONSE_BODY:
            raise ValueError("响应消息体长度超出允许范围")
        body = recv_exact(connection, length)
        result = json.loads(body.decode("utf-8"))
        if not isinstance(result, dict) or result.get("request_id") != request_id:
            raise ValueError("JSON 结果与请求编号不一致")
        expected_status = "ok" if kind == 2 else "error"
        if result.get("status") != expected_status:
            raise ValueError("响应类型与 JSON 状态不一致")
        print("收到处理结果：" if kind == 2 else "收到错误响应：")
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if kind == 2 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, struct.error) as error:
        print(f"客户端失败：{error}", file=sys.stderr)
        sys.exit(1)
