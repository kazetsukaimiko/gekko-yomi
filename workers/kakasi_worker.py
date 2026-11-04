#!/usr/bin/env python3
"""Persistent pykakasi worker.

One JSON object per line on stdin: {"text": "..."}.
One JSON object per line on stdout. pykakasi is not installed by the build.
The dictionary form is the surface. This worker does not lemmatize.
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


def morphemes_for(converter, text):
    morphemes = []
    char_at = 0
    for item in converter.convert(text):
        surface = item.get("orig") or ""
        found = text.find(surface, char_at) if surface else char_at
        if found < 0:
            found = char_at
        start = len(text[:found].encode("utf-8"))
        end = start + len(surface.encode("utf-8"))
        reading = item.get("hira") or ""
        if reading == surface:
            reading = ""
        morphemes.append(
            {
                "surface": surface,
                "reading": reading,
                "part_of_speech": "",
                "dictionary_form": surface,
                "start": start,
                "end": end,
            }
        )
        char_at = found + len(surface)
    return morphemes


def main():
    try:
        import pykakasi
    except Exception as exc:
        sys.stdout.buffer.write(
            (json.dumps({"error": "kakasi worker failed to import: " + str(exc)}) + "\n").encode("utf-8")
        )
        sys.stdout.buffer.flush()
        return 1

    converter = pykakasi.kakasi()
    stream = sys.stdin.buffer
    while True:
        header = read_line(stream)
        if header is None:
            return 0
        try:
            meta = json.loads(header)
            payload = {"morphemes": morphemes_for(converter, meta.get("text") or "")}
        except Exception as exc:
            payload = {"error": "kakasi worker failed: " + str(exc)}
        sys.stdout.buffer.write((json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8"))
        sys.stdout.buffer.flush()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(0)
