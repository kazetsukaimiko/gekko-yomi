# Gekko Yomi roadmap

Living notes for what we are building, in what order. The overlay, speech, and the kanji log are real goals. They are not the first milestone.

## What this is

A Linux app that reads Japanese off a window you pick and shows the text with furigana and English glosses.

The machine this is aimed at first is KDE Plasma on Wayland (KWin 6.8), with a Radeon RX 7900 XTX. A second machine with a 9070 XT can run OCR later. The app process stays on the desktop that owns the window.

## MVP

A web UI on a local port.

1. Open the page.
2. Pick a window from a dropdown.
3. The app grabs that window and finds the Japanese text blocks in it.
4. The page shows those blocks: the original text, furigana when we have a reading, and a short gloss when we have a dictionary.

One capture implementation, for this machine. One OCR implementation, to start. The other models plug in behind the same interface.

Qt is not required for the web page. A normal build also includes a system-tray icon and a small settings window (`GEKKO_YOMI_QT_SHELL` defaults to ON). `-DGEKKO_YOMI_QT_SHELL=OFF` remains for a core-only build. The core library does not link Qt in either case.

## Not the MVP

Written down so they are not forgotten and not built early.

- **Overlay.** A click-through layer on top of the target window that paints furigana on the kanji, matched to fill, outline, gradient, size, and a nearby font. Whole-window mode and under-cursor mode. This wants KWin's layer-shell (the `LayerShellQt` library is already on this machine) and `QPainter` stroke/fill. It is a later component, behind the overlay interface, and it can stay unbuilt forever without breaking the web UI.
- **Speech.** Speak the line or the kanji on demand. `SpeechSynth` is the interface. No engine in the MVP.
- **Kanji log.** A small embedded database of what was seen, so older entries can be looked up. `KanjiLog` is the interface. No database in the MVP.
- **Tablet UI, a browser packaged as the desktop app, Rust.** Not planned.

## Interfaces

The core is C++17. Plugins are shared libraries that export `gekko_yomi_register(Host*)`. The host does not know about KWin, ONNX, HTTP, or Qt. Implementations register themselves.

| Interface | Role | First implementation |
| --- | --- | --- |
| `CaptureSource` | List windows and grab pixels | KWin `ScreenShot2` on this machine |
| `OcrEngine` | Image to text, with character boxes when the model has them | One local engine (meikiocr) |
| `LanguageModel` | OpenAI-compatible chat, optional image | llama-run, and any other server that speaks that API |
| `StyleSampler` | Fill, outline, gradient, font class from a glyph crop | In-process sampler, when the overlay exists |
| `ReadingProvider` | Surface string to morphemes and kana | MeCab + UniDic, later a Sudachi plugin if we want it |
| `Dictionary` | Dictionary form to English glosses | JMdict |
| `Presenter` | Choose which readings to draw (all, or under the cursor) | Used by the overlay |
| `Overlay` | Paint placed readings on a click-through surface | layer-shell, optional |
| `SpeechSynth` | Speak a string | Later |
| `KanjiLog` | Append and search past lookups | Embedded database, later |

`image_rect_to_overlay` is the only place that turns a box in the captured image into a position on the screen. Scale and window origin live in that function. Nothing else multiplies by scale.

### Capture: KWin first, PipeWire later

On this session, KWin `org.kde.KWin.ScreenShot2` `CaptureWindow` returns the window's own pixels, including when another window covers it, without a portal picker. A window grab is on the order of a few milliseconds. That is the MVP `CaptureSource`.

The caller has to be a real binary whose `.desktop` file lists `X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2`. The C++ capture plugin can be that binary's helper. A Python interpreter cannot.

PipeWire and the xdg-desktop-portal ScreenCast interface are the later `CaptureSource` for compositors that are not KWin. The web UI talks to `CaptureSource` only, so swapping the grabber does not change the page.

### OCR and llama-run

These models are planned. They are not all in the first feature PR.

- meikiocr
- PP-OCRv6_manga
- manga-ocr-nar-small
- manga-ocr-charspot-nano
- a PaddleOCR Japanese model (PP-OCRv6 small or medium, ONNX, not the Paddle runtime)
- **llama-run, through the OpenAI API.** A vision model behind `LanguageModel` can also be wrapped as an `OcrEngine`. Boxes may be missing. `OcrEngine::provides_character_boxes()` tells the UI that. The overlay needs boxes. The web UI can show a string without them.

Character boxes are the geometry for furigana. meiki's recognizer and charspot-nano detect characters. Line-only models vote the text. They do not place the reading.

An OCR engine does not report font or color. `StyleSampler` reads those from the pixels in the box. That matters for the overlay, not for the first web page.

Weights stay out of git. nar-small and charspot-nano are CC BY-NC-SA 4.0 (personal use, do not redistribute the weights). meiki weights are LGPL-3.0. The PP-OCRv6 models we want are Apache-2.0. JMdict stays attributed in the UI.

The OCR worker may be a separate process at first, because the published decode code is Python and the system Python is 3.14. That process is an `OcrEngine` plugin. The core does not import Python. A native ONNX plugin can replace it later per model. GPU inference, when we turn it on, pins the discrete card (RADV NAVI31 here, the 9070 XT on the other machine) and refuses the Raphael iGPU.

### Optional Qt shell

A tray icon that opens a small settings window. The default build includes it (`GEKKO_YOMI_QT_SHELL=ON`). Pass `-DGEKKO_YOMI_QT_SHELL=OFF` to skip it. The icon is a crescent made of plus marks. Settings that the web UI already covers do not move into Qt as a second source of truth. The shell opens the page and edits the buffer knobs and which OCR engines run.

## Pull requests

1. **Interfaces and the build.** This one. The project builds. Running it with no plugins exits with an error that says nothing is implemented. Qt is not on the link line.
2. **Tests and GitHub Actions.** Platform-agnostic tests for the core (registry, geometry, result types, fakes for each interface). A workflow runs them in Docker. A second, optional job may run Wayland-specific scenarios under a headless or nested KWin. Those scenarios call the same tests with a platform fixture. Missing a Wayland socket skips them. It does not fail the agnostic job.
3. **KWin capture**, plus tests that do not need a real compositor (a fake `CaptureSource`) and one scenario that does.
4. **Web UI and the first `OcrEngine`.** Pick a window, grab it, show the text blocks. Manual read first. A poll loop comes after a single read is trustworthy.
5. **llama-run / OpenAI `LanguageModel`**, and an `OcrEngine` that uses it.
6. **The other OCR models**, still behind `OcrEngine`, with a consensus step in the core.
7. **Readings and the dictionary**, surfaced on the same page.
8. **Kanji log.** Embedded database. Not before the page works.
9. **Qt tray and settings window.** Default build includes it. `GEKKO_YOMI_QT_SHELL=OFF` leaves it out.
10. **Overlay**, then under-cursor mode.
11. **Speech.**

Each feature PR after the test harness lands with tests for that feature.

## Tests, the shape we want

Platform-agnostic tests live next to the core and run anywhere the binary builds. They use fakes: a capture source that returns a fixed image, an OCR engine that returns fixed boxes.

Platform scenarios are extra cases, labeled so CI can separate them. Example: the same "boxes on this image land on these overlay coordinates" test, fed once by a synthetic `FrameMapping` and once by a real KWin grab of a marker window. The assertion does not change. The fixture does.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/gekko-yomi
```

That last command serves the page and, when a display is set, puts a tray icon up. `-DGEKKO_YOMI_QT_SHELL=OFF` builds the page without the tray.
