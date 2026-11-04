#!/usr/bin/env python3
"""Test double for the kakasi worker. One morpheme, reading よみ, for the whole line."""

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
        text = meta.get("text") or ""
        payload = {
            "morphemes": [
                {
                    "surface": text,
                    "reading": "よみ",
                    "part_of_speech": "",
                    "dictionary_form": text,
                    "start": 0,
                    "end": len(text.encode("utf-8")),
                }
            ]
        }
        sys.stdout.buffer.write((json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"))
        sys.stdout.buffer.flush()


if __name__ == "__main__":
    raise SystemExit(main())
