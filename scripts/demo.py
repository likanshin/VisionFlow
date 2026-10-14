"""Run a self-contained demo and benchmark; always stop the child server."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, default=Path("build/debug/bin"))
    args = parser.parse_args()
    binary = args.bin_dir.resolve()
    report = ROOT / "reports" / time.strftime("demo-%Y%m%d-%H%M%S")
    report.mkdir(parents=True, exist_ok=False)
    def run(*command):
        subprocess.run([str(part) for part in command], check=True, cwd=ROOT)
    run(sys.executable, ROOT / "python/generate_sample.py", "--output", report / "input")
    run(binary / "vision_cli", "--input", report / "input/frame.jpg", "--processor", "lane_lines",
        "--output", report / "frame_result.json", "--preview", report / "frame_preview.jpg")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    config = json.loads((ROOT / "configs/server.json").read_text())
    config.update(port=port, mode="offline")
    config_path = report / "server.json"
    config_path.write_text(json.dumps(config, indent=2))
    with (report / "server.log").open("w") as log:
        server = subprocess.Popen([str(binary / "vision_server"), "--config", str(config_path)], cwd=ROOT, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if server.poll() is not None:
                    raise RuntimeError("server exited: see " + str(report / "server.log"))
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                raise TimeoutError("server startup timed out")
            run(sys.executable, ROOT / "python/client.py", "--port", port, "--video", report / "input/sample.avi",
                "--record", report / "recording", "--max-frames", 30)
            run(sys.executable, ROOT / "python/replay.py", report / "recording", "--output", report / "replay.avi")
            run(sys.executable, ROOT / "python/benchmark.py", "--port", port, "--requests", 100,
                "--clients", 2, "--window", 4, "--output", report / "benchmark.json")
        finally:
            if server.poll() is None:
                server.terminate()
            try:
                server.wait(timeout=12)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        if server.returncode:
            raise RuntimeError("server exit code: " + str(server.returncode))
    print("Demo artifacts:", report)


if __name__ == "__main__":
    main()
