"""End-to-end tests against real C++ subprocesses and TCP sockets."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))
from protocol import HEADER, encode, receive, stream


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class RunningServer:
    def __init__(self, **settings):
        self.directory = tempfile.TemporaryDirectory()
        config = json.loads((ROOT / "configs/server.json").read_text())
        config.update(port=free_port(), idle_timeout_ms=1500, io_timeout_ms=1200)
        config.update(settings)
        self.port = config["port"]
        self.path = Path(self.directory.name) / "server.json"
        self.path.write_text(json.dumps(config))
        self.log = open(Path(self.directory.name) / "server.log", "w+")
        self.process = subprocess.Popen([str(BIN / "vision_server"), "--config", str(self.path)], stdout=self.log, stderr=self.log)
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    self.log.seek(0)
                    raise RuntimeError(self.log.read())
                try:
                    with self.connect():
                        return
                except OSError:
                    time.sleep(0.03)
            raise TimeoutError("server startup")
        except BaseException:
            self.close()
            raise

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=5)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.flush()
        self.log.seek(0)
        diagnostic = self.log.read()[-8000:]
        exit_code = self.process.returncode
        self.log.close()
        self.directory.cleanup()
        return exit_code, diagnostic

    def __enter__(self):
        return self

    def __exit__(self, exc_type, *_):
        exit_code, diagnostic = self.close()
        if exc_type is None and exit_code != 0:
            raise AssertionError("server exit {}:\n{}".format(exit_code, diagnostic))


class Integration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        image = np.zeros((360, 640, 3), np.uint8)
        cv2.line(image, (70, 359), (285, 200), (255, 255, 255), 5)
        cv2.line(image, (570, 359), (355, 200), (255, 255, 255), 5)
        cls.jpeg = cv2.imencode(".jpg", image)[1].tobytes()

    def test_fragmented_coalesced_and_invalid_image(self):
        with RunningServer(mode="offline") as server, server.connect() as sock:
            packet = encode(1, self.jpeg)
            for i in range(0, len(packet), 7):
                sock.sendall(packet[i:i + 7])
            self.assertEqual(receive(sock)[2]["status"], "ok")
            sock.sendall(encode(2, self.jpeg) + encode(3, b"not a jpeg") + encode(4, self.jpeg))
            results = {request_id: result for _, request_id, result in [receive(sock) for _ in range(3)]}
            self.assertEqual(results[3]["code"], "invalid_image")
            self.assertEqual(results[2]["status"], "ok")
            self.assertEqual(results[4]["width"], 640)

    def test_invalid_headers_close_only_offending_connection(self):
        with RunningServer() as server:
            headers = [
                HEADER.pack(b"NOPE", 1, 1, 1, 100, 0),
                HEADER.pack(b"VFLW", 2, 1, 1, 100, 0),
                HEADER.pack(b"VFLW", 1, 9, 1, 100, 0),
                HEADER.pack(b"VFLW", 1, 1, 1, 0xffffffff, 0),
                HEADER.pack(b"VFLW", 1, 1, 1, 0, 0),
                HEADER.pack(b"VFLW", 1, 1, 1, 100, 1),
            ]
            for header in headers:
                with server.connect() as sock:
                    sock.sendall(header)
                    self.assertEqual(sock.recv(1), b"")
            with server.connect() as sock:
                sock.sendall(encode(99, self.jpeg))
                self.assertEqual(receive(sock)[1], 99)

    def test_offline_backpressure_multiclient_and_reconnect(self):
        # Leave room for disconnected sockets until epoll observes their closure.
        # This test exercises session isolation, not the connection admission limit.
        with RunningServer(mode="offline", worker_threads=1, task_queue_capacity=1, max_inflight_per_client=2, max_clients=16) as server:
            def client(index):
                replies = list(stream("127.0.0.1", server.port, ((i, self.jpeg) for i in range(1, 21)), window=8))
                self.assertEqual({row[0] for row in replies}, set(range(1, 21)))
                self.assertTrue(all(row[1]["status"] == "ok" for row in replies))
            with ThreadPoolExecutor(max_workers=3) as executor:
                list(executor.map(client, range(3)))
            for _ in range(8):
                with server.connect() as sock:
                    sock.sendall(encode(987654, self.jpeg))
            with server.connect() as sock:
                sock.sendall(encode(123, self.jpeg))
                self.assertEqual(receive(sock)[1], 123)

    def test_realtime_drop_notifications(self):
        # Large deterministic noise keeps one worker busy while many tiny frames queue.
        large = np.random.RandomState(42).randint(0, 256, (1080, 1920, 3), dtype=np.uint8)
        jpeg = cv2.imencode(".jpg", large)[1].tobytes()
        with RunningServer(worker_threads=1, task_queue_capacity=1, max_inflight_per_client=64) as server:
            with server.connect() as sock:
                sock.sendall(encode(1, jpeg))
                sock.sendall(b"".join(encode(i, self.jpeg) for i in range(2, 41)))
                replies = [receive(sock) for _ in range(40)]
                self.assertEqual({r[1] for r in replies}, set(range(1, 41)))
                self.assertTrue(any(r[2].get("code") == "dropped" for r in replies))
                self.assertTrue(all(r[2]["status"] == "ok" or r[2].get("code") == "dropped" for r in replies))

    def test_slow_partial_client_timeout_and_other_client(self):
        with RunningServer(io_timeout_ms=300) as server, server.connect() as slow:
            slow.sendall(b"VFL")
            with server.connect() as fast:
                fast.sendall(encode(1, self.jpeg))
                self.assertEqual(receive(fast)[2]["status"], "ok")
            time.sleep(0.5)
            self.assertEqual(slow.recv(1), b"")

    def test_shutdown_drains_accepted_jobs(self):
        with RunningServer(mode="offline") as server, server.connect() as sock:
            sock.sendall(encode(1, self.jpeg))
            self.assertEqual(receive(sock)[1], 1)
            server.process.send_signal(signal.SIGTERM)
            self.assertEqual(server.process.wait(timeout=8), 0)

    def test_nonreading_client_does_not_block_others(self):
        with RunningServer(mode="offline", max_send_buffer_bytes=65536, max_inflight_per_client=256,
                           io_timeout_ms=300, log_level="error") as server, server.connect() as slow:
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            slow.settimeout(2)
            def flood():
                try:
                    for i in range(3000):
                        slow.sendall(encode(i + 1, self.jpeg))
                except OSError:
                    pass  # Closing/backpressure is expected for this nonreading peer.
            sender = threading.Thread(target=flood)
            sender.start()
            try:
                time.sleep(0.2)
                replies = list(stream("127.0.0.1", server.port, ((i, self.jpeg) for i in range(1, 11)), window=2))
                self.assertEqual(len(replies), 10)
                self.assertTrue(all(row[1]["status"] == "ok" for row in replies))
            finally:
                slow.close()
                sender.join(timeout=3)
                self.assertFalse(sender.is_alive())

    def test_stop_during_active_processing(self):
        image = np.random.RandomState(7).randint(0, 256, (1080, 1920, 3), dtype=np.uint8)
        jpeg = cv2.imencode(".jpg", image)[1].tobytes()
        with RunningServer(mode="offline", worker_threads=1) as server, server.connect() as sock:
            sock.sendall(encode(1, jpeg) + encode(2, jpeg))
            time.sleep(0.03)
            server.process.terminate()
            self.assertEqual(server.process.wait(timeout=8), 0)

    def test_python_record_and_replay(self):
        with tempfile.TemporaryDirectory() as temp, RunningServer(mode="offline") as server:
            temp = Path(temp)
            commands = [
                [sys.executable, str(ROOT / "python/generate_sample.py"), "--output", str(temp / "input")],
                [sys.executable, str(ROOT / "python/client.py"), "--port", str(server.port), "--video", str(temp / "input/sample.avi"),
                 "--record", str(temp / "recording"), "--max-frames", "5"],
                [sys.executable, str(ROOT / "python/replay.py"), str(temp / "recording"), "--output", str(temp / "replay.avi")],
            ]
            for command in commands:
                run = subprocess.run(command, capture_output=True, timeout=15)
                self.assertEqual(run.returncode, 0, run.stderr.decode())
            entries = [json.loads(line) for line in (temp / "recording/results.jsonl").read_text().splitlines()]
            self.assertEqual({entry["request_id"] for entry in entries}, set(range(1, 6)))
            capture = cv2.VideoCapture(str(temp / "replay.avi"))
            try:
                self.assertEqual(int(capture.get(cv2.CAP_PROP_FRAME_COUNT)), 5)
                self.assertTrue(capture.read()[0])
            finally:
                capture.release()

    def test_cli_and_configuration_errors(self):
        with tempfile.TemporaryDirectory() as temp:
            image, output = Path(temp) / "input.jpg", Path(temp) / "result.json"
            image.write_bytes(self.jpeg)
            run = subprocess.run([str(BIN / "vision_cli"), "--input", str(image), "--output", str(output), "--processor", "edges"], capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertGreater(json.loads(output.read_text())["edge_pixels"], 0)
            run = subprocess.run([str(BIN / "vision_cli"), "--input", str(image) + ".missing", "--output", str(output)], capture_output=True)
            self.assertNotEqual(run.returncode, 0)
            for config in ({"worker_threads": -1}, {"typo": 2}, {"max_pixels": -1}, {"port": 1.5}):
                path = Path(temp) / "config.json"
                path.write_text(json.dumps(config))
                run = subprocess.run([str(BIN / "vision_server"), "--config", str(path)], capture_output=True, timeout=5)
                self.assertNotEqual(run.returncode, 0)

    def test_supervisor_status_and_signal(self):
        run = subprocess.run([str(BIN / "vision_supervisor"), "/bin/sh", "-c", "exit 7"], capture_output=True, timeout=5)
        self.assertEqual(run.returncode, 7)
        run = subprocess.run([str(BIN / "vision_supervisor"), "/no/such/program"], capture_output=True, timeout=5)
        self.assertNotEqual(run.returncode, 0)
        child = subprocess.Popen([str(BIN / "vision_supervisor"), "/bin/sleep", "30"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            self.assertIn(b"child_pid=", child.stderr.readline())
            child.terminate()
            self.assertEqual(child.wait(timeout=5), 128 + signal.SIGTERM)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
            child.stderr.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", required=True, type=Path)
    args, rest = parser.parse_known_args()
    BIN = args.bin_dir.resolve()
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)
