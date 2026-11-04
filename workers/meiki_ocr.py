#!/usr/bin/env python3
"""Persistent meikiocr worker.

One JSON object per line on stdin, then exactly stride*height raw BGRA bytes.
One JSON object per line on stdout. Logs stay on stderr. Weights are downloaded
by meikiocr on first use and are not part of this repository.

provider=None lets onnxruntime choose. Do not pass a ROCm provider. Pinning the
discrete GPU is a later step; the default wheel is CPU.
"""

import json
import sys


def read_line(stream):
    data = bytearray()
    while True:
        chunk = stream.read(1)
        if not chunk:
            return None
        if chunk == b"\n":
            return data.decode("utf-8")
        data.extend(chunk)


def handle(engine, meta, pixels):
    import numpy as np

    width = int(meta["width"])
    height = int(meta["height"])
    stride = int(meta["stride"])
    image = np.frombuffer(pixels, dtype=np.uint8).reshape(height, stride)
    bgra = image[:, : width * 4].reshape(height, width, 4)
    bgr = bgra[:, :, :3]
    runs = []
    for block in engine.run_ocr(bgr):
        characters = []
        left = top = right = bottom = None
        for item in block.get("chars") or []:
            bbox = item.get("bbox") or [0, 0, 0, 0]
            x1, y1, x2, y2 = [float(v) for v in bbox[:4]]
            if x2 < x1:
                x1, x2 = x2, x1
            if y2 < y1:
                y1, y2 = y2, y1
            characters.append(
                {
                    "text": item.get("char") or "",
                    "box": [x1, y1, x2 - x1, y2 - y1],
                    "confidence": float(item.get("conf") or 0),
                }
            )
            left = x1 if left is None else min(left, x1)
            top = y1 if top is None else min(top, y1)
            right = x2 if right is None else max(right, x2)
            bottom = y2 if bottom is None else max(bottom, y2)
        box = [0, 0, 0, 0]
        if left is not None:
            box = [left, top, right - left, bottom - top]
        runs.append(
            {
                "text": block.get("text") or "",
                "vertical": bool(block.get("is_vertical")),
                "confidence": 0,
                "box": box,
                "characters": characters,
            }
        )
    return {"runs": runs}


def main():
    try:
        from meikiocr import MeikiOCR
    except Exception as exc:
        sys.stdout.buffer.write(
            (json.dumps({"error": "meiki worker failed to import: " + str(exc)}) + "\n").encode("utf-8")
        )
        sys.stdout.buffer.flush()
        return 1

    # provider=None: CUDA if that build has it, otherwise CPU. No ROCm provider.
    engine = MeikiOCR(provider=None)
    stream = sys.stdin.buffer
    while True:
        header = read_line(stream)
        if header is None:
            return 0
        try:
            meta = json.loads(header)
            nbytes = int(meta["stride"]) * int(meta["height"])
            pixels = stream.read(nbytes)
            if len(pixels) != nbytes:
                payload = {"error": "short pixel read"}
            else:
                payload = handle(engine, meta, pixels)
        except Exception as exc:
            payload = {"error": "meiki worker failed: " + str(exc)}
        sys.stdout.buffer.write((json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"))
        sys.stdout.buffer.flush()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(0)
