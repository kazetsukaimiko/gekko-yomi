#!/usr/bin/env python3
"""Test double for the meiki worker protocol. No model, no weights."""

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


def main():
    stream = sys.stdin.buffer
    while True:
        header = read_line(stream)
        if header is None:
            return 0
        meta = json.loads(header)
        nbytes = int(meta["stride"]) * int(meta["height"])
        pixels = stream.read(nbytes)
        if len(pixels) != nbytes:
            payload = {"error": "short pixel read"}
        else:
            payload = {
                "runs": [
                    {
                        "text": "漢字",
                        "vertical": False,
                        "confidence": 0.9,
                        "box": [0, 0, int(meta["width"]), int(meta["height"])],
                        "characters": [
                            {"text": "漢", "box": [0, 0, 1, 1], "confidence": 0.9},
                            {"text": "字", "box": [1, 0, 1, 1], "confidence": 0.9},
                        ],
                    }
                ]
            }
        sys.stdout.buffer.write((json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"))
        sys.stdout.buffer.flush()


if __name__ == "__main__":
    raise SystemExit(main())
