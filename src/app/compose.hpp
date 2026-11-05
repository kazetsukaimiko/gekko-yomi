#pragma once

#include "gekko_yomi/frame.hpp"
#include "gekko_yomi/registry.hpp"

#include <string>
#include <vector>

namespace gekko::yomi {

struct MorphemeView {
    std::string surface;
    std::string reading;
    std::size_t start_byte{0};
    std::size_t end_byte{0};
    std::vector<std::string> glosses;
};

struct CharView {
    std::string text;
    Rect box;
};

struct RunView {
    std::string text;
    Rect box;
    bool vertical{false};
    std::vector<CharView> characters;
    // Empty when no ReadingProvider is loaded. The page then shows text as-is.
    std::vector<MorphemeView> morphemes;
};

struct EngineMilliseconds {
    std::string id;
    int milliseconds{0};
};

// Wall time for one read. ocr_ms is the wait for every engine, which is the
// slowest engine when they run together. engines is each engine's own time.
struct ScanTimings {
    int list_ms{0};
    int capture_ms{0};
    int ocr_ms{0};
    int buffer_ms{0};
    int language_ms{0};
    int total_ms{0};
    std::vector<EngineMilliseconds> engines;
};

struct ReadView {
    std::string capture_error;
    std::string ocr_error;
    // Set when a reading or dictionary plugin is loaded and that call fails.
    // An absent plugin leaves these empty; the page still shows the OCR text.
    std::string reading_error;
    std::string dictionary_error;
    std::string capture_id;
    std::string engine_id;
    std::string window_id;
    std::string title;
    bool minimized{false};
    int image_width{0};
    int image_height{0};
    Frame frame;
    std::vector<RunView> runs;
    // True when a scan buffer has seen text and has not promoted it yet.
    bool settling{false};
    ScanTimings timings;
};

class OcrConsensus;

// First capture source, then every OCR engine. Engines run together when
// more than one is loaded. Latin letters (ASCII and fullwidth) are removed
// before anything is remembered. A run that is only numbers or symbols is
// dropped; digits and marks stay when the same run still has kana or kanji.
// When memory is set, each engine updates its
// own buffer and a consensus buffer merges what those buffers show. A null
// memory returns the filtered runs of this scan directly. Readings and
// glosses are filled only when those plugins are loaded. A missing plugin
// leaves its field empty. A loaded plugin that fails sets reading_error or
// dictionary_error. The grabbed frame is still returned.
ReadView read_window(Registry& registry, std::string_view window_id, OcrConsensus* memory = nullptr);

std::string windows_json(Registry& registry);
std::string read_json(const ReadView& view);

}  // namespace gekko::yomi
