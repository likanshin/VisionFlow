"""VFLW v1 framing and bounded, out-of-order TCP request streaming."""
import json
import select
import socket
import struct
import time
from collections import deque

HEADER = struct.Struct("!4sHHQII")
MAX_PAYLOAD = 16 * 1024 * 1024
MAX_RESPONSE = 65536


def encode(request_id, jpeg):
    if not 0 < len(jpeg) <= MAX_PAYLOAD:
        raise ValueError("JPEG payload outside 1..16 MiB")
    return HEADER.pack(b"VFLW", 1, 1, request_id, len(jpeg), 0) + jpeg


def parse_header(data, limit=MAX_RESPONSE):
    magic, version, kind, request_id, length, flags = HEADER.unpack(data)
    if magic != b"VFLW" or version != 1 or flags or kind not in (1, 2, 3):
        raise ValueError("invalid VFLW header")
    if not 0 < length <= limit:
        raise ValueError("payload length outside limit")
    return kind, request_id, length


def recv_exact(sock, length):
    result = bytearray()
    while len(result) < length:
        chunk = sock.recv(length - len(result))
        if not chunk:
            raise ConnectionError("server closed connection")
        result.extend(chunk)
    return bytes(result)


def receive(sock):
    kind, request_id, length = parse_header(recv_exact(sock, HEADER.size))
    if kind not in (2, 3):
        raise ValueError("expected server response")
    result = json.loads(recv_exact(sock, length).decode("utf-8"))
    if result.get("request_id") != request_id:
        raise ValueError("response request_id mismatch")
    return kind, request_id, result


def stream(host, port, images, window=4, rate=0.0, timeout=15.0):
    """Yield (id, JSON, latency_ms, JPEG); images yields (unique id, JPEG).

    Both outgoing and pending frames are bounded by window. A single thread
    services partial writes and reads, so a full send buffer cannot deadlock it.
    rate is per-connection frames/s; 0 runs as fast as the window permits.
    """
    if not 1 <= window <= 256 or rate < 0 or timeout <= 0:
        raise ValueError("invalid window, rate or timeout")
    source = iter(images)
    pending, outgoing, incoming = {}, deque(), bytearray()
    exhausted, offset, next_send = False, 0, time.monotonic()
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.setblocking(False)
        while pending or not exhausted:
            now = time.monotonic()
            if not exhausted and len(pending) < window and now >= next_send:
                try:
                    request_id, jpeg = next(source)
                except StopIteration:
                    exhausted = True
                else:
                    if request_id in pending:
                        raise ValueError("duplicate pending request_id")
                    packet = encode(request_id, jpeg)
                    pending[request_id] = (time.monotonic(), jpeg)
                    outgoing.append(packet)
                    next_send = time.monotonic() + (1.0 / rate if rate else 0)
            if not pending and exhausted:
                break
            if pending and now - min(item[0] for item in pending.values()) > timeout:
                raise TimeoutError("request deadline exceeded")
            wait = 0.05
            if not exhausted and len(pending) < window:
                wait = max(0.0, min(wait, next_send - time.monotonic()))
            readable, writable, _ = select.select([sock], [sock] if outgoing else [], [], wait)
            if writable:
                try:
                    count = sock.send(memoryview(outgoing[0])[offset:])
                    if not count:
                        raise ConnectionError("send returned zero")
                    offset += count
                    if offset == len(outgoing[0]):
                        outgoing.popleft()
                        offset = 0
                except BlockingIOError:
                    pass
            if readable:
                try:
                    chunk = sock.recv(65536)
                except BlockingIOError:
                    continue
                if not chunk:
                    raise ConnectionError("server closed with outstanding requests")
                incoming.extend(chunk)
                while len(incoming) >= HEADER.size:
                    kind, request_id, length = parse_header(incoming[:HEADER.size])
                    if kind not in (2, 3):
                        raise ValueError("expected server response")
                    if len(incoming) < HEADER.size + length:
                        break
                    result = json.loads(incoming[HEADER.size:HEADER.size + length].decode("utf-8"))
                    del incoming[:HEADER.size + length]
                    if result.get("request_id") != request_id or request_id not in pending:
                        raise ValueError("unknown or mismatched response id")
                    started, jpeg = pending.pop(request_id)
                    yield request_id, result, (time.monotonic() - started) * 1000, jpeg
