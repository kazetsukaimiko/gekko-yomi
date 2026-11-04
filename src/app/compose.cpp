#include "compose.hpp"

#include "json_mini.hpp"

#include <utility>

namespace gekko::yomi {
namespace {

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

void attach_language(Registry& registry, RunView* run) {
    if (registry.reading_providers().empty() || run->text.empty()) {
        return;
    }
    const auto analyzed = registry.reading_providers().front()->analyze(run->text);
    if (!analyzed.ok() || !analyzed.value) {
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
            }
        }
        run->morphemes.push_back(std::move(view));
    }
}

}  // namespace

ReadView read_window(Registry& registry, std::string_view window_id) {
    ReadView view;
    view.window_id = std::string(window_id);
    if (registry.capture_sources().empty()) {
        view.capture_error = "no capture plugin is loaded";
        return view;
    }
    CaptureSource* capture = registry.capture_sources().front().get();
    view.capture_id = capture->id();

    const auto windows = capture->list_windows();
    if (windows.ok() && windows.value) {
        for (const auto& window : *windows.value) {
            if (window.id == window_id) {
                view.title = window.title;
                view.minimized = window.minimized;
                break;
            }
        }
    }

    auto grabbed = capture->grab_window(window_id);
    if (!grabbed.ok() || !grabbed.value) {
        view.capture_error = grabbed.error.empty() ? "capture failed" : grabbed.error;
        return view;
    }
    view.image_width = grabbed.value->width;
    view.image_height = grabbed.value->height;
    view.frame = std::move(*grabbed.value);

    if (registry.ocr_engines().empty()) {
        view.ocr_error = "no OCR engine is loaded";
        return view;
    }
    OcrEngine* engine = registry.ocr_engines().front().get();
    view.engine_id = engine->id();
    const auto pass = engine->recognize(view.frame, {});
    if (!pass.ok() || !pass.value) {
        view.ocr_error = pass.error.empty() ? "OCR failed" : pass.error;
        return view;
    }
    for (const auto& run : pass.value->runs) {
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
        attach_language(registry, &item);
        view.runs.push_back(std::move(item));
    }
    return view;
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
