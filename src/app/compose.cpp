#include "compose.hpp"

#include "gekko_yomi/ocr_buffer.hpp"
#include "json_mini.hpp"
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace gekko::yomi {
namespace {

std::size_t codepoints(std::string_view text) {
    std::size_t count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

void append_utf8(std::string* out, char32_t cp) {
    if (cp <= 0x7F) {
        out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// A bad byte is kept as itself so a broken sequence is not dropped on the floor.
bool decode_utf8(std::string_view text, std::size_t* index, char32_t* cp) {
    if (*index >= text.size()) {
        return false;
    }
    const auto lead = static_cast<unsigned char>(text[*index]);
    if (lead < 0x80) {
        *cp = lead;
        *index += 1;
        return true;
    }
    int extra = 0;
    char32_t value = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07;
    } else {
        *cp = lead;
        *index += 1;
        return true;
    }
    if (*index + static_cast<std::size_t>(extra) >= text.size()) {
        *cp = lead;
        *index += 1;
        return true;
    }
    for (int step = 1; step <= extra; ++step) {
        const auto next = static_cast<unsigned char>(text[*index + static_cast<std::size_t>(step)]);
        if ((next & 0xC0) != 0x80) {
            *cp = lead;
            *index += 1;
            return true;
        }
        value = (value << 6) | (next & 0x3F);
    }
    *cp = value;
    *index += static_cast<std::size_t>(extra) + 1;
    return true;
}

bool is_latin_letter(char32_t cp) {
    return (cp >= U'A' && cp <= U'Z') || (cp >= U'a' && cp <= U'z') || (cp >= 0xFF21 && cp <= 0xFF3A) ||
           (cp >= 0xFF41 && cp <= 0xFF5A);
}

bool is_space(char32_t cp) {
    return cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r' || cp == 0x3000 || cp == 0x00A0;
}

// A run is worth keeping only when kana or kanji remains. Digits and symbols
// stay in that run, but a candidate that is only numbers or symbols is dropped.
bool is_kana(char32_t cp) {
    if (cp >= 0x3041 && cp <= 0x3096) {
        return true;
    }
    // Katakana letters. Skip the middle dot; a mark by itself is not a reading.
    if (cp >= 0x30A1 && cp <= 0x30FA && cp != 0x30FB) {
        return true;
    }
    if (cp >= 0xFF66 && cp <= 0xFF9D) {
        return true;
    }
    return cp >= 0x1B000 && cp <= 0x1B16F;
}

bool is_kanji(char32_t cp) {
    if (cp >= 0x3400 && cp <= 0x4DBF) {
        return true;
    }
    if (cp >= 0x4E00 && cp <= 0x9FFF) {
        return true;
    }
    if (cp >= 0xF900 && cp <= 0xFAFF) {
        return true;
    }
    return cp >= 0x20000 && cp <= 0x3FFFF;
}

bool is_reading_content(char32_t cp) {
    return is_kana(cp) || is_kanji(cp);
}

struct FilteredText {
    std::string text;
    bool keep{false};
};

FilteredText filter_english(std::string_view text) {
    std::vector<char32_t> points;
    std::size_t index = 0;
    char32_t cp = 0;
    while (decode_utf8(text, &index, &cp)) {
        if (!is_latin_letter(cp)) {
            points.push_back(cp);
        }
    }
    std::size_t begin = 0;
    while (begin < points.size() && is_space(points[begin])) {
        ++begin;
    }
    std::size_t end = points.size();
    while (end > begin && is_space(points[end - 1])) {
        --end;
    }
    FilteredText filtered;
    bool previous_space = false;
    for (std::size_t i = begin; i < end; ++i) {
        if (is_reading_content(points[i])) {
            filtered.keep = true;
        }
        if (is_space(points[i])) {
            if (previous_space) {
                continue;
            }
            previous_space = true;
            filtered.text.push_back(' ');
            continue;
        }
        previous_space = false;
        append_utf8(&filtered.text, points[i]);
    }
    return filtered;
}

std::optional<TextRun> without_english(const TextRun& run) {
    const FilteredText text = filter_english(run.text);
    if (!text.keep) {
        return std::nullopt;
    }
    TextRun kept = run;
    kept.text = text.text;
    kept.characters.clear();
    for (const auto& character : run.characters) {
        const FilteredText piece = filter_english(character.text);
        if (!piece.keep) {
            continue;
        }
        CharBox box = character;
        box.text = piece.text;
        kept.characters.push_back(std::move(box));
    }
    return kept;
}

struct EngineOutcome {
    std::string id;
    std::string error;
    int milliseconds{0};
    std::vector<TextRun> runs;
};

int elapsed_ms(std::chrono::steady_clock::time_point started) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
}

EngineOutcome recognize_one(OcrEngine* engine, const Frame* frame) {
    const auto started = std::chrono::steady_clock::now();
    EngineOutcome outcome;
    outcome.id = engine->id();
    try {
        const auto pass = engine->recognize(*frame, {});
        if (!pass.ok() || !pass.value) {
            outcome.error = pass.error.empty() ? "OCR failed" : pass.error;
        } else {
            for (const auto& run : pass.value->runs) {
                if (auto kept = without_english(run)) {
                    outcome.runs.push_back(std::move(*kept));
                }
            }
        }
    } catch (const std::exception& ex) {
        outcome.error = ex.what();
    }
    outcome.milliseconds = elapsed_ms(started);
    return outcome;
}

std::vector<EngineOutcome> recognize_all(const std::vector<std::unique_ptr<OcrEngine>>& engines, const Frame& frame) {
    if (engines.size() <= 1) {
        if (engines.empty()) {
            return {};
        }
        return {recognize_one(engines.front().get(), &frame)};
    }
    std::vector<std::future<EngineOutcome>> jobs;
    jobs.reserve(engines.size());
    for (const auto& engine : engines) {
        OcrEngine* raw = engine.get();
        jobs.push_back(std::async(std::launch::async, [raw, &frame]() { return recognize_one(raw, &frame); }));
    }
    std::vector<EngineOutcome> outcomes;
    outcomes.reserve(jobs.size());
    for (auto& job : jobs) {
        outcomes.push_back(job.get());
    }
    return outcomes;
}

void append_box(std::string* out, const Rect& box) {
    *out += '[';
    *out += json_number(box.x);
    *out += ',';
    *out += json_number(box.y);
    *out += ',';
    *out += json_number(box.width);
    *out += ',';
    *out += json_number(box.height);
    *out += ']';
}

void attach_language(Registry& registry, RunView* run, std::string* reading_error,
                     std::string* dictionary_error) {
    if (registry.reading_providers().empty() || run->text.empty()) {
        return;
    }
    const auto analyzed = registry.reading_providers().front()->analyze(run->text);
    if (!analyzed.ok() || !analyzed.value) {
        if (reading_error->empty()) {
            *reading_error = analyzed.error.empty() ? "reading failed" : analyzed.error;
        }
        return;
    }
    Dictionary* dictionary =
        registry.dictionaries().empty() ? nullptr : registry.dictionaries().front().get();
    for (const auto& morpheme : *analyzed.value) {
        MorphemeView view;
        view.surface = morpheme.surface;
        view.reading = morpheme.reading;
        view.start_byte = morpheme.start_byte;
        view.end_byte = morpheme.end_byte;
        if (dictionary != nullptr) {
            const std::string key =
                morpheme.dictionary_form.empty() ? morpheme.surface : morpheme.dictionary_form;
            const auto glosses = dictionary->lookup(key);
            if (glosses.ok() && glosses.value) {
                for (const auto& gloss : *glosses.value) {
                    view.glosses.insert(view.glosses.end(), gloss.english.begin(), gloss.english.end());
                }
            } else if (dictionary_error->empty()) {
                *dictionary_error = glosses.error.empty() ? "dictionary lookup failed" : glosses.error;
            }
        }
        run->morphemes.push_back(std::move(view));
    }
}

}  // namespace

ReadView read_window(Registry& registry, std::string_view window_id, OcrConsensus* memory) {
    const auto started = std::chrono::steady_clock::now();
    ReadView view;
    view.window_id = std::string(window_id);
    auto finish = [&]() {
        view.timings.total_ms = elapsed_ms(started);
        return view;
    };
    if (registry.capture_sources().empty()) {
        view.capture_error = "no capture plugin is loaded";
        return finish();
    }
    CaptureSource* capture = registry.capture_sources().front().get();
    view.capture_id = capture->id();

    const auto listed_at = std::chrono::steady_clock::now();
    const auto windows = capture->list_windows();
    view.timings.list_ms = elapsed_ms(listed_at);
    if (windows.ok() && windows.value) {
        for (const auto& window : *windows.value) {
            if (window.id == window_id) {
                view.title = window.title;
                view.minimized = window.minimized;
                break;
            }
        }
    }

    const auto grabbed_at = std::chrono::steady_clock::now();
    auto grabbed = capture->grab_window(window_id);
    view.timings.capture_ms = elapsed_ms(grabbed_at);
    if (!grabbed.ok() || !grabbed.value) {
        view.capture_error = grabbed.error.empty() ? "capture failed" : grabbed.error;
        return finish();
    }
    view.image_width = grabbed.value->width;
    view.image_height = grabbed.value->height;
    view.frame = std::move(*grabbed.value);

    if (registry.ocr_engines().empty()) {
        view.ocr_error = "no OCR engine is loaded";
        return finish();
    }
    std::string engine_ids;
    for (const auto& engine : registry.ocr_engines()) {
        if (!engine_ids.empty()) {
            engine_ids += '+';
        }
        engine_ids += engine->id();
    }
    view.engine_id = std::move(engine_ids);

    const auto recognized_at = std::chrono::steady_clock::now();
    const auto outcomes = recognize_all(registry.ocr_engines(), view.frame);
    view.timings.ocr_ms = elapsed_ms(recognized_at);
    std::vector<EngineScan> scans;
    scans.reserve(outcomes.size());
    std::string failures;
    int succeeded = 0;
    for (const auto& outcome : outcomes) {
        if (!outcome.error.empty()) {
            logger("ocr").error("{}: {}", outcome.id, outcome.error);
            if (!failures.empty()) {
                failures += "; ";
            }
            failures += outcome.id;
            failures += ": ";
            failures += outcome.error;
        } else {
            ++succeeded;
            logger("ocr").debug("{} runs {}", outcome.id, outcome.runs.size());
        }
        view.timings.engines.push_back(EngineMilliseconds{outcome.id, outcome.milliseconds});
        scans.push_back(EngineScan{outcome.id, outcome.runs});
    }

    std::vector<TextRun> admitted;
    const auto buffered_at = std::chrono::steady_clock::now();
    if (memory != nullptr) {
        admitted = memory->observe(scans);
        view.settling = memory->settling() && admitted.empty();
    } else {
        for (const auto& scan : scans) {
            admitted.insert(admitted.end(), scan.runs.begin(), scan.runs.end());
        }
    }
    view.timings.buffer_ms = elapsed_ms(buffered_at);
    if (succeeded == 0 && !failures.empty()) {
        view.ocr_error = failures;
    }
    const auto read_at = std::chrono::steady_clock::now();
    for (const auto& run : admitted) {
        RunView item;
        item.text = run.text;
        item.box = run.box;
        item.vertical = run.vertical;
        for (const auto& character : run.characters) {
            CharView box;
            box.text = character.text;
            box.box = character.box;
            item.characters.push_back(std::move(box));
        }
        attach_language(registry, &item, &view.reading_error, &view.dictionary_error);
        view.runs.push_back(std::move(item));
    }
    // Longest line first. Equal lengths keep the order the buffer produced.
    std::stable_sort(view.runs.begin(), view.runs.end(), [](const RunView& left, const RunView& right) {
        return codepoints(left.text) > codepoints(right.text);
    });
    view.timings.language_ms = elapsed_ms(read_at);
    return finish();
}

std::string windows_json(Registry& registry) {
    std::string out = "{\"load_errors\":[";
    for (std::size_t i = 0; i < registry.load_errors().size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += json_quote(registry.load_errors()[i]);
    }
    out += "],\"capture\":";
    if (registry.capture_sources().empty()) {
        out += "\"\",\"capture_error\":\"no capture plugin is loaded\",\"windows\":[]}";
        return out;
    }
    CaptureSource* capture = registry.capture_sources().front().get();
    out += json_quote(capture->id());
    out += ",\"capture_error\":";
    const auto windows = capture->list_windows();
    if (!windows.ok() || !windows.value) {
        out += json_quote(windows.error.empty() ? "capture failed" : windows.error);
        out += ",\"windows\":[]}";
        return out;
    }
    out += "\"\",\"windows\":[";
    bool first = true;
    for (const auto& window : *windows.value) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"id\":";
        out += json_quote(window.id);
        out += ",\"title\":";
        out += json_quote(window.title);
        out += ",\"app_id\":";
        out += json_quote(window.app_id);
        out += ",\"minimized\":";
        out += window.minimized ? "true" : "false";
        out += ",\"x\":";
        out += json_number(window.geometry.x);
        out += ",\"y\":";
        out += json_number(window.geometry.y);
        out += ",\"width\":";
        out += json_number(window.geometry.width);
        out += ",\"height\":";
        out += json_number(window.geometry.height);
        out += '}';
    }
    out += "]}";
    return out;
}

std::string read_json(const ReadView& view) {
    std::string out = "{\"capture_error\":";
    out += json_quote(view.capture_error);
    out += ",\"ocr_error\":";
    out += json_quote(view.ocr_error);
    out += ",\"reading_error\":";
    out += json_quote(view.reading_error);
    out += ",\"dictionary_error\":";
    out += json_quote(view.dictionary_error);
    out += ",\"capture\":";
    out += json_quote(view.capture_id);
    out += ",\"engine\":";
    out += json_quote(view.engine_id);
    out += ",\"window_id\":";
    out += json_quote(view.window_id);
    out += ",\"title\":";
    out += json_quote(view.title);
    out += ",\"minimized\":";
    out += view.minimized ? "true" : "false";
    out += ",\"image_width\":";
    out += std::to_string(view.image_width);
    out += ",\"image_height\":";
    out += std::to_string(view.image_height);
    out += ",\"settling\":";
    out += view.settling ? "true" : "false";
    out += ",\"timings_ms\":{\"list\":";
    out += std::to_string(view.timings.list_ms);
    out += ",\"capture\":";
    out += std::to_string(view.timings.capture_ms);
    out += ",\"ocr\":";
    out += std::to_string(view.timings.ocr_ms);
    out += ",\"buffer\":";
    out += std::to_string(view.timings.buffer_ms);
    out += ",\"language\":";
    out += std::to_string(view.timings.language_ms);
    out += ",\"total\":";
    out += std::to_string(view.timings.total_ms);
    out += ",\"engines\":[";
    for (std::size_t i = 0; i < view.timings.engines.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += "{\"id\":";
        out += json_quote(view.timings.engines[i].id);
        out += ",\"ms\":";
        out += std::to_string(view.timings.engines[i].milliseconds);
        out += '}';
    }
    out += "]}";
    out += ",\"runs\":[";
    for (std::size_t i = 0; i < view.runs.size(); ++i) {
        const auto& run = view.runs[i];
        if (i != 0) {
            out += ',';
        }
        out += "{\"text\":";
        out += json_quote(run.text);
        out += ",\"vertical\":";
        out += run.vertical ? "true" : "false";
        out += ",\"box\":";
        append_box(&out, run.box);
        out += ",\"characters\":[";
        for (std::size_t c = 0; c < run.characters.size(); ++c) {
            if (c != 0) {
                out += ',';
            }
            out += "{\"text\":";
            out += json_quote(run.characters[c].text);
            out += ",\"box\":";
            append_box(&out, run.characters[c].box);
            out += '}';
        }
        out += "],\"morphemes\":[";
        for (std::size_t m = 0; m < run.morphemes.size(); ++m) {
            const auto& morpheme = run.morphemes[m];
            if (m != 0) {
                out += ',';
            }
            out += "{\"surface\":";
            out += json_quote(morpheme.surface);
            out += ",\"reading\":";
            out += json_quote(morpheme.reading);
            out += ",\"start\":";
            out += std::to_string(morpheme.start_byte);
            out += ",\"end\":";
            out += std::to_string(morpheme.end_byte);
            out += ",\"glosses\":[";
            for (std::size_t g = 0; g < morpheme.glosses.size(); ++g) {
                if (g != 0) {
                    out += ',';
                }
                out += json_quote(morpheme.glosses[g]);
            }
            out += "]}";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

}  // namespace gekko::yomi
