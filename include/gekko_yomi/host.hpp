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
#include <string>

namespace gekko::yomi {

// Facts the host copies out of the plugin before register returns.
// Pointers here are only read during the register call.
struct PluginDescriptor {
    const char* id{nullptr};
    const char* description{nullptr};
};

struct OcrDescriptor {
    const char* id{nullptr};
    const char* description{nullptr};
    bool character_boxes{false};
};

// Capture-less function pointers. The host calls them while the .so is mapped.
// A null unique_ptr declines. The error string says why. It is not a dlopen failure.
using CaptureFactory = std::unique_ptr<CaptureSource> (*)(std::string* error);
using OcrFactory = std::unique_ptr<OcrEngine> (*)(std::string* error);
using LanguageModelFactory = std::unique_ptr<LanguageModel> (*)(std::string* error);
using StyleSamplerFactory = std::unique_ptr<StyleSampler> (*)(std::string* error);
using ReadingFactory = std::unique_ptr<ReadingProvider> (*)(std::string* error);
using DictionaryFactory = std::unique_ptr<Dictionary> (*)(std::string* error);
using PresenterFactory = std::unique_ptr<Presenter> (*)(std::string* error);
using OverlayFactory = std::unique_ptr<Overlay> (*)(std::string* error);
using SpeechFactory = std::unique_ptr<SpeechSynth> (*)(std::string* error);
using KanjiLogFactory = std::unique_ptr<KanjiLog> (*)(std::string* error);

// Plugins register a factory. The host creates the instance.
// add is for objects the host or an in-process test already built.
class Host {
public:
    virtual ~Host() = default;

    virtual void register_capture(PluginDescriptor descriptor, CaptureFactory create) = 0;
    virtual void register_ocr(OcrDescriptor descriptor, OcrFactory create) = 0;
    virtual void register_language_model(PluginDescriptor descriptor, LanguageModelFactory create) = 0;
    virtual void register_style_sampler(PluginDescriptor descriptor, StyleSamplerFactory create) = 0;
    virtual void register_reading(PluginDescriptor descriptor, ReadingFactory create) = 0;
    virtual void register_dictionary(PluginDescriptor descriptor, DictionaryFactory create) = 0;
    virtual void register_presenter(PluginDescriptor descriptor, PresenterFactory create) = 0;
    virtual void register_overlay(PluginDescriptor descriptor, OverlayFactory create) = 0;
    virtual void register_speech(PluginDescriptor descriptor, SpeechFactory create) = 0;
    virtual void register_kanji_log(PluginDescriptor descriptor, KanjiLogFactory create) = 0;

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
