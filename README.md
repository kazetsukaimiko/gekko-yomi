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

This build produces `plugins/capture_kwin.so`, `plugins/ocr_meiki.so`, and `plugins/ocr_ppocr_manga.so`. The capture plugin lists normal and dialog windows through KWin and returns their pixels as BGRA. Each OCR plugin speaks to its own Python worker, `workers/meiki_ocr.py` or `workers/ppocr_manga.py`. Both run on a grab, and the page shows what their buffers agree on.

```bash
./build/gekko-yomi
```

That listens on `0.0.0.0` port 8765 (or the next free port up to 8775) and prints `http://127.0.0.1:PORT` on stdout. Another machine on the network uses this host's address and the same port. The page's poll menu rereads the chosen window every 50, 100, 250, 500, or 1000 ms. The default is 1000 ms. Plugin load and each grab are logged to stderr with spdlog. A line is the time, the logger name (`app`, `plugin`, `http`, `capture`, `ocr`, `reading`, or `dictionary`), the level, and the message. `GEKKO_YOMI_LOG` sets the level (`debug`, `info`, `warn`, `error`); the default is `info`. The page lists windows, grabs the one you pick, and shows the text when an OCR engine answers. After that it reads the same window again at the poll interval. Latin letters are removed from the text. A run that is only letters, numbers, or symbols is dropped. Digits and marks stay when the same run still has kana or kanji. Each loaded OCR engine keeps its own buffer of recent hits. A consensus buffer merges what those buffers are willing to show, including when only one engine is loaded. A line appears after the same reading wins two scans, so a one-off fragment stays off the page. Overlapping hits vote, and the longer reading wins a tie. Furigana and glosses appear when a reading plugin and a dictionary are loaded. Without them the page still shows the OCR text, or the grabbed image with a note when OCR is missing.

meikiocr is not installed by the build. If `GEKKO_YOMI_PYTHON` is unset, the worker uses `<executable>/../.venv/bin/python` when that file exists, and otherwise `python3`. That environment should be Python 3.12 with meikiocr installed. The system Python here is 3.14, which does not have the wheels. Weights download on first use and stay out of git. `GEKKO_YOMI_MEIKI_WORKER` overrides the worker script.

`plugins/ocr_ppocr_manga.so` runs the Apache-2.0 PP-OCRv6 manga models (`Kellenok/PP-OCRv6_manga`, FP32 ONNX) through onnxruntime. It needs `pyclipper` in that same interpreter. There is no Paddle runtime and no ROCm provider. The model returns a line box, not a character box. Weights download on first use. `GEKKO_YOMI_PPOCR_MANGA_WORKER` overrides `workers/ppocr_manga.py`.

`plugins/reading_kakasi.so` asks `workers/kakasi_worker.py` for kana. `GEKKO_YOMI_KAKASI_WORKER` overrides that script. The worker needs `pykakasi` in the same interpreter. It does not lemmatize: the dictionary form is the surface you see, so a TSV keyed by that surface will match. A later plugin can fill in conjugation.

`plugins/dictionary_tsv.so` reads `GEKKO_YOMI_DICTIONARY`, a UTF-8 TSV you supply. A line is `form`, a tab, `part of speech`, a tab, and the gloss. Two columns mean the second column is the gloss. Several senses in one gloss are split on ` / `. A line starting with `#` is a comment. The file stays out of git. If the table comes from JMdict, keep the Electronic Dictionary Research and Development Group attribution with it. When the variable is unset the plugin does not register, and the page still shows the reading.
