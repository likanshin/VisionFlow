"""Read video, send bounded JPEG requests, optionally preview and record."""
import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np

from protocol import stream


def overlay(frame, result, latency_ms=0):
    frame = frame.copy()
    for x1, y1, x2, y2 in result.get("lines", []):
        cv2.line(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)
    label = "{}  id={}  {:.1f} ms".format(result.get("status"), result.get("request_id"), latency_ms)
    cv2.putText(frame, label, (12, 25), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 2)
    return frame


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--video", required=True)
    parser.add_argument("--window", type=int, default=4)
    parser.add_argument("--fps", type=float, default=0, help="send rate; 0 = unlimited")
    parser.add_argument("--max-frames", type=int, default=0)
    parser.add_argument("--quality", type=int, default=85)
    parser.add_argument("--width", type=int, default=1280, help="downscale larger frames, preserve aspect ratio")
    parser.add_argument("--display", action="store_true", help="requires desktop/OpenCV GUI")
    parser.add_argument("--record", type=Path, help="new recording directory")
    args = parser.parse_args()
    if not 1 <= args.quality <= 100 or args.width < 1 or args.max_frames < 0:
        parser.error("invalid quality, width or frame count")
    cv2.setNumThreads(1)
    cap = cv2.VideoCapture(args.video)
    if not cap.isOpened():
        raise ValueError("cannot open video: " + args.video)
    record = None
    try:
        if args.record:
            args.record.mkdir(parents=True, exist_ok=False)
            (args.record / "frames").mkdir()
            record = (args.record / "results.jsonl").open("w", encoding="utf-8")
        def images():
            count = 0
            while not args.max_frames or count < args.max_frames:
                ok, frame = cap.read()
                if not ok:
                    break
                if frame.shape[1] > args.width:
                    frame = cv2.resize(frame, (args.width, max(1, round(frame.shape[0] * args.width / frame.shape[1]))))
                ok, encoded = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, args.quality])
                if not ok:
                    raise ValueError("JPEG encoding failed")
                count += 1
                yield count, encoded.tobytes()
        results = stream(args.host, args.port, images(), args.window, args.fps)
        try:
            for request_id, result, latency, jpeg in results:
                entry = {"request_id": request_id, "latency_ms": latency, "result": result}
                if record:
                    relative = "frames/{:08d}.jpg".format(request_id)
                    (args.record / relative).write_bytes(jpeg)
                    entry["frame"] = relative
                    record.write(json.dumps(entry, ensure_ascii=False) + "\n")
                    record.flush()
                print(json.dumps(entry, ensure_ascii=False))
                if args.display:
                    frame = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
                    cv2.imshow("VisionFlow", overlay(frame, result, latency))
                    if cv2.waitKey(1) & 0xFF == ord("q"):
                        break
        finally:
            results.close()
    finally:
        cap.release()
        if record:
            record.close()
        if args.display:
            cv2.destroyAllWindows()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, cv2.error) as error:
        print("client:", error, file=sys.stderr)
        sys.exit(1)
