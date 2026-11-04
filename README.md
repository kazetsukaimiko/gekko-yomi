# Gekko Yomi

Reads Japanese text from a window you pick and shows the original line, furigana, and a short English gloss.

The first milestone is a web page on a local port. An overlay, speech, and a history database are later. See [docs/roadmap.md](docs/roadmap.md).

This tree builds a core library, a local web page, and plugins. The page still serves when no plugins are loaded and says what is missing. The Qt tray and settings window is not in the tree yet, so `GEKKO_YOMI_QT_SHELL` is off and turning it on is an error. Once that window exists, a normal build includes it, and `-DGEKKO_YOMI_QT_SHELL=OFF` skips it. The core library does not link Qt.

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/gekko-yomi
```

Plugins are shared libraries that export `gekko_yomi_register`. Put them in `plugins/` next to the executable, or in `../lib/gekko-yomi/plugins`. Set `GEKKO_YOMI_PLUGINS` to a colon-separated directory list to use those directories and no others.

This build produces `plugins/capture_kwin.so` and `plugins/ocr_meiki.so`. The capture plugin lists normal and dialog windows through KWin and returns their pixels as BGRA. The OCR plugin speaks to a Python worker, `workers/meiki_ocr.py`.

```bash
./build/gekko-yomi
```

That listens on `127.0.0.1` port 8765 (or the next free port up to 8775) and prints the URL. The page lists windows, grabs the one you pick, and shows the text when an OCR engine answers. Furigana and glosses appear when a reading plugin and a dictionary are loaded. Without them the page still shows the OCR text, or the grabbed image with a note when OCR is missing.

meikiocr is not installed by the build. Point `GEKKO_YOMI_PYTHON` at a Python 3.12 virtualenv that has it. The system Python here is 3.14, which does not have the wheels. Weights download on first use and stay out of git. `GEKKO_YOMI_MEIKI_WORKER` overrides the worker script.
