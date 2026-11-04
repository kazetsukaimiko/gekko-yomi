#pragma once

#include "gekko_yomi/capture.hpp"
#include "gekko_yomi/dictionary.hpp"
#include "gekko_yomi/kanji_log.hpp"
#include "gekko_yomi/language_model.hpp"
#include "gekko_yomi/ocr.hpp"
#include "gekko_yomi/overlay.hpp"
#include "gekko_yomi/reading.hpp"
#include "gekko_yomi/speech.hpp"
#include "gekko_yomi/style.hpp"

#include <memory>

namespace gekko::yomi {

// Plugins hand the host implementations. The host owns them after add returns.
// add is called on the thread that loads plugins, before the app serves traffic.
class Host {
public:
    virtual ~Host() = default;

    virtual void add(std::unique_ptr<CaptureSource> source) = 0;
    virtual void add(std::unique_ptr<OcrEngine> engine) = 0;
    virtual void add(std::unique_ptr<LanguageModel> model) = 0;
    virtual void add(std::unique_ptr<StyleSampler> sampler) = 0;
    virtual void add(std::unique_ptr<ReadingProvider> provider) = 0;
    virtual void add(std::unique_ptr<Dictionary> dictionary) = 0;
    virtual void add(std::unique_ptr<Presenter> presenter) = 0;
    virtual void add(std::unique_ptr<Overlay> overlay) = 0;
    virtual void add(std::unique_ptr<SpeechSynth> speech) = 0;
    virtual void add(std::unique_ptr<KanjiLog> log) = 0;
};

// Each plugin .so exports this symbol with default visibility.
// Compile plugins against these headers. Link gekko_yomi_core only if the
// plugin calls a non-inline function such as image_rect_to_overlay.
#define GEKKO_YOMI_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))

}  // namespace gekko::yomi
