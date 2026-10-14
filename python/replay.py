"""Replay JSONL recording in request order; optionally export a video."""
import argparse
import json
import sys
from pathlib import Path

import cv2

from client import overlay


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--output", type=Path, help="annotated .avi video")
    parser.add_argument("--display", action="store_true")
    parser.add_argument("--fps", type=float, default=25)
    args = parser.parse_args()
    if args.fps <= 0:
        parser.error("fps must be positive")
    root = args.recording.resolve()
    with (root / "results.jsonl").open(encoding="utf-8") as source:
        entries = sorted((json.loads(line) for line in source if line.strip()), key=lambda row: row["request_id"])
    writer = None
    try:
        for entry in entries:
            path = (root / entry["frame"]).resolve()
            if root not in path.parents:
                raise ValueError("recorded frame path escapes recording directory")
            image = cv2.imread(str(path))
            if image is None:
                raise ValueError("missing recorded frame: " + str(path))
            image = overlay(image, entry["result"], entry["latency_ms"])
            if args.output:
                if writer is None:
                    args.output.parent.mkdir(parents=True, exist_ok=True)
                    size = (image.shape[1], image.shape[0])
                    writer = cv2.VideoWriter(str(args.output), cv2.VideoWriter_fourcc(*"MJPG"), args.fps, size)
                    if not writer.isOpened():
                        raise ValueError("cannot open output video")
                if (image.shape[1], image.shape[0]) != size:
                    raise ValueError("recording contains mixed image sizes")
                writer.write(image)
            if args.display:
                cv2.imshow("VisionFlow replay", image)
                if cv2.waitKey(max(1, round(1000 / args.fps))) & 0xFF == ord("q"):
                    break
        print(json.dumps({"frames": len(entries), "errors": sum(e["result"].get("status") != "ok" for e in entries)}))
    finally:
        if writer:
            writer.release()
        if args.display:
            cv2.destroyAllWindows()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, cv2.error) as error:
        print("replay:", error, file=sys.stderr)
        sys.exit(1)
