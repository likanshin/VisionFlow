"""Create deterministic synthetic road footage; no downloaded assets needed."""
import argparse
from pathlib import Path
import cv2
import numpy as np


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("data"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    writer = cv2.VideoWriter(str(args.output / "sample.avi"), cv2.VideoWriter_fourcc(*"MJPG"), 25, (640, 360))
    if not writer.isOpened():
        raise RuntimeError("MJPG video writer is unavailable")
    try:
        for i in range(100):
            frame = np.full((360, 640, 3), 35, np.uint8)
            frame[:185] = (170, 140, 100)
            offset = int(12 * np.sin(i / 20))
            cv2.line(frame, (75 + offset, 359), (285 + offset, 200), (255, 255, 255), 6)
            cv2.line(frame, (565 + offset, 359), (355 + offset, 200), (0, 230, 255), 6)
            if i == 0 and not cv2.imwrite(str(args.output / "frame.jpg"), frame):
                raise RuntimeError("cannot write sample JPEG")
            writer.write(frame)
    finally:
        writer.release()
    print("Generated", args.output / "sample.avi", "and", args.output / "frame.jpg")


if __name__ == "__main__":
    main()
