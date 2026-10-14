"""Bounded multi-client benchmark; all latency clocks are client monotonic clocks."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import platform
import sys
import time
from pathlib import Path

import cv2
import numpy as np
from protocol import stream


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--clients", type=int, default=2)
    parser.add_argument("--requests", type=int, default=100, help="measured requests per client")
    parser.add_argument("--warmup", type=int, default=10, help="warmup requests per client")
    parser.add_argument("--window", type=int, default=4)
    parser.add_argument("--rate", type=float, default=0, help="target frames/s per client; 0 = unlimited")
    parser.add_argument("--output", type=Path, default=Path("reports/benchmark.json"))
    args = parser.parse_args()
    if args.clients < 1 or args.requests < 1 or args.warmup < 0:
        parser.error("invalid clients, requests or warmup")
    cv2.setNumThreads(1)
    if args.image:
        jpeg = args.image.read_bytes()
    else:
        image = np.zeros((360, 640, 3), np.uint8)
        cv2.line(image, (60, 359), (285, 200), (255, 255, 255), 5)
        cv2.line(image, (580, 359), (355, 200), (255, 255, 255), 5)
        ok, encoded = cv2.imencode(".jpg", image)
        if not ok:
            raise ValueError("JPEG encoding failed")
        jpeg = encoded.tobytes()
    def run(count):
        return [(result, latency) for _, result, latency, _ in stream(args.host, args.port,
                ((i + 1, jpeg) for i in range(count)), args.window, args.rate)]
    with ThreadPoolExecutor(max_workers=args.clients) as executor:
        if args.warmup:
            list(executor.map(run, [args.warmup] * args.clients))
        start = time.monotonic()
        samples = [sample for group in executor.map(run, [args.requests] * args.clients) for sample in group]
        elapsed = time.monotonic() - start
    successes = [latency for result, latency in samples if result["status"] == "ok"]
    error_codes = Counter(result.get("code", "unknown") for result, _ in samples if result["status"] != "ok")
    report = {
        "environment": {"platform": platform.platform(), "python": platform.python_version(), "opencv": cv2.__version__},
        "settings": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
        "payload_bytes": len(jpeg), "duration_s": elapsed, "responses": len(samples), "successes": len(successes),
        "successful_fps": len(successes) / elapsed, "error_rate": 1 - len(successes) / len(samples),
        "errors": dict(error_codes),
        "success_latency_ms": dict(zip(("p50", "p95", "p99"), np.percentile(successes, [50, 95, 99]).tolist())) if successes else None,
        "all_response_latency_ms": dict(zip(("p50", "p95", "p99"), np.percentile([s[1] for s in samples], [50, 95, 99]).tolist())),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if successes else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, cv2.error) as error:
        print("benchmark:", error, file=sys.stderr)
        sys.exit(1)
