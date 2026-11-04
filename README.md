# Gekko Yomi

Reads Japanese text from a window you pick and shows the original line, furigana, and a short English gloss.

The first milestone is a web page on a local port. An overlay, speech, and a history database are later. See [docs/roadmap.md](docs/roadmap.md).

This tree builds a core library and a process that loads plugins. With no plugins present it exits and says so. The Qt tray and settings window is not in the tree yet, so `GEKKO_YOMI_QT_SHELL` is off and turning it on is an error. Once that window exists, a normal build includes it, and `-DGEKKO_YOMI_QT_SHELL=OFF` skips it. The core library does not link Qt.

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/gekko-yomi
```

Plugins are shared libraries that export `gekko_yomi_register`. Put them in `GEKKO_YOMI_PLUGINS` (colon-separated directories), or in `plugins/` next to the executable.
