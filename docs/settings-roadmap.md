# Settings roadmap

The scan buffer and the OCR engines are configurable without a rebuild. The Qt window that edits these settings is not in the tree yet. Until it is, `GEKKO_YOMI_QT_SHELL` stays OFF, and turning it on is a configure error. The core library does not link Qt.

## Config file

JSON. The default path is `~/.config/gekko-yomi/core.json`, from `HOME`. `XDG_CONFIG_HOME` is not used. The Qt settings window, when it exists, reads and writes this same file. It is not a second source of truth.

```bash
./build/gekko-yomi
./build/gekko-yomi --config /path/to/core.json
```

`--config` is parsed with [CLI11](https://github.com/CLIUtils/CLI11) 2.7.2 (header-only, fetched by CMake). The flag is on the app binary. The core library does not parse argv.

- No `--config`, and the default file is missing: built-in defaults. The file is not created. An info line says so.
- No `--config`, and `HOME` is unset: built-in defaults, with a warning.
- No `--config`, and the default file exists but is not valid JSON: the process exits with an error.
- `--config PATH` missing, unreadable, or not valid JSON: the process exits with an error.
- Unknown keys are ignored, with a warning, so a typo does not look like it took effect.

`GEKKO_YOMI_PLUGINS` still decides which plugin libraries load. The config decides which of the loaded OCR engines run.

## Knobs in this step

```json
{
  "buffer": {
    "ttl_scans": 8,
    "positional_invalidation_scans": 2
  },
  "ocr": {
    "engines": ["meiki", "ppocr-manga"]
  }
}
```

Leave a key out to keep its default. The defaults match the behavior from before this file existed.

| Key | Default | Meaning |
| --- | --- | --- |
| `buffer.ttl_scans` | 8 | How many scans a reading may vote, including the scan it was seen on. This is not a timer. After that, the hit stays in memory for the promotion streak, but it no longer adds a vote. A short value lets new text replace old text in the same box. Positional removal does not do that, because the new text still occupies the box. `1` counts only the scan that just finished. The line on the page still changes only after it wins two scans. |
| `buffer.positional_invalidation_scans` | 2 | Consecutive scans with no recognition in that box before the group, its reading, and its preview boxes are dropped. `2` keeps the group through one empty scan and drops it on the second. Any overlapping hit resets the count. One engine still seeing the box is enough. |
| `ocr.engines` | absent | Ids of loaded OCR engines that run. Omit the key and every loaded engine runs. `[]` runs none. An id that is not loaded is a warning. A listed engine is not called, so it does not spend CPU and does not age the buffer. Plugins still load and are still logged. |

`ppocr-manga` stays enabled unless the file says otherwise. To leave it loaded but idle:

```json
{
  "ocr": {
    "engines": ["meiki"]
  }
}
```

Engine ids today are `meiki` and `ppocr-manga`.

## Later

Module plugins keep their own files at `~/.config/gekko-yomi/modules.d/<module_name>.json`. Nothing reads that directory yet. `core.json` stays the core file: buffer and which engines run. A module does not grow keys inside `core.json`.

The Qt tray and settings window edits `core.json` and, later, the module files. It opens the page and exposes the knobs above. When that target exists, `GEKKO_YOMI_QT_SHELL` defaults to ON. `-DGEKKO_YOMI_QT_SHELL=OFF` remains the core-only build. The core still does not link Qt.

Not in the file yet, and not moved into Qt as a second copy: the page's poll interval (it stays in `localStorage`), the port (`GEKKO_YOMI_PORT`), overlap, history length, and how many scans a line must win before it is shown. GPU device selection is also later. When inference leaves the CPU, it pins the discrete card and refuses the Raphael iGPU.
