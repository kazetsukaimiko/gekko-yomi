#pragma once

#include "gekko_yomi/host.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gekko_yomi/result.hpp"

namespace gekko::yomi {

// Loads plugin directories and keeps what they registered.
class Registry final : public Host {
public:
    Registry();
    ~Registry() override;

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // If GEKKO_YOMI_PLUGINS is set, only those colon-separated directories.
    // Otherwise <exe>/plugins, then <exe>/../lib/gekko-yomi/plugins.
    // A missing directory is skipped. A library that fails to load is recorded
    // in load_errors and the rest still load.
    void load();

    std::size_t implementation_count() const;
    const std::vector<std::string>& load_errors() const { return load_errors_; }
    // Paths of .so files that are still mapped, in load order.
    std::vector<std::string> module_paths() const;

    // Destroys that module's instances, drops its factories, then dlclose.
    // Fails while a scan is on the stack, or when path is not loaded.
    Status unload(const std::string& path);

    // Stop and destroy the live instance with this id. The .so and the factory
    // stay, so create_instance can build it again. Fails during a scan, or
    // when that id has no live instance.
    Status release_instance(const std::string& id);

    // Call the stored factory. Fails during a scan, when the id was not
    // registered, when an instance is already live, or when the factory declines.
    Status create_instance(const std::string& id);

    // read_window holds a scan for its whole call. Unload and instance
    // changes are refused in between.
    void begin_scan();
    void end_scan();

    // When set before load, only these OCR ids are constructed. The other
    // factories stay, so a later create does not dlopen again. An empty list
    // constructs none.
    void limit_ocr_engines(std::vector<std::string> ids);

    const std::vector<std::unique_ptr<CaptureSource>>& capture_sources() const {
        return capture_sources_;
    }
    const std::vector<std::unique_ptr<OcrEngine>>& ocr_engines() const { return ocr_engines_; }
    const std::vector<std::unique_ptr<LanguageModel>>& language_models() const {
        return language_models_;
    }
    const std::vector<std::unique_ptr<StyleSampler>>& style_samplers() const {
        return style_samplers_;
    }
    const std::vector<std::unique_ptr<ReadingProvider>>& reading_providers() const {
        return reading_providers_;
    }
    const std::vector<std::unique_ptr<Dictionary>>& dictionaries() const { return dictionaries_; }
    const std::vector<std::unique_ptr<Presenter>>& presenters() const { return presenters_; }
    const std::vector<std::unique_ptr<Overlay>>& overlays() const { return overlays_; }
    const std::vector<std::unique_ptr<SpeechSynth>>& speech_synths() const { return speech_synths_; }
    const std::vector<std::unique_ptr<KanjiLog>>& kanji_logs() const { return kanji_logs_; }

    void add(std::unique_ptr<CaptureSource> source) override;
    void add(std::unique_ptr<OcrEngine> engine) override;
    void add(std::unique_ptr<LanguageModel> model) override;
    void add(std::unique_ptr<StyleSampler> sampler) override;
    void add(std::unique_ptr<ReadingProvider> provider) override;
    void add(std::unique_ptr<Dictionary> dictionary) override;
    void add(std::unique_ptr<Presenter> presenter) override;
    void add(std::unique_ptr<Overlay> overlay) override;
    void add(std::unique_ptr<SpeechSynth> speech) override;
    void add(std::unique_ptr<KanjiLog> log) override;

    void register_capture(PluginDescriptor descriptor, CaptureFactory create) override;
    void register_ocr(OcrDescriptor descriptor, OcrFactory create) override;
    void register_language_model(PluginDescriptor descriptor, LanguageModelFactory create) override;
    void register_style_sampler(PluginDescriptor descriptor, StyleSamplerFactory create) override;
    void register_reading(PluginDescriptor descriptor, ReadingFactory create) override;
    void register_dictionary(PluginDescriptor descriptor, DictionaryFactory create) override;
    void register_presenter(PluginDescriptor descriptor, PresenterFactory create) override;
    void register_overlay(PluginDescriptor descriptor, OverlayFactory create) override;
    void register_speech(PluginDescriptor descriptor, SpeechFactory create) override;
    void register_kanji_log(PluginDescriptor descriptor, KanjiLogFactory create) override;

private:
    struct Module;
    struct Modules;

    void load_directory(const std::string& directory);
    void instantiate(Module& module);
    void drop_module(Module& module);
    bool id_taken(const std::string& id) const;
    bool prepare_id(const char* id, const char* kind);
    template <typename Slot, typename Factory>
    void register_named(std::vector<Slot>* slots, PluginDescriptor descriptor, Factory create, const char* kind);

    std::unique_ptr<Modules> modules_;
    Module* loading_{nullptr};
    int scan_depth_{0};
    std::optional<std::vector<std::string>> ocr_limit_;
    std::vector<std::string> load_errors_;
    std::vector<std::unique_ptr<CaptureSource>> capture_sources_;
    std::vector<std::unique_ptr<OcrEngine>> ocr_engines_;
    std::vector<std::unique_ptr<LanguageModel>> language_models_;
    std::vector<std::unique_ptr<StyleSampler>> style_samplers_;
    std::vector<std::unique_ptr<ReadingProvider>> reading_providers_;
    std::vector<std::unique_ptr<Dictionary>> dictionaries_;
    std::vector<std::unique_ptr<Presenter>> presenters_;
    std::vector<std::unique_ptr<Overlay>> overlays_;
    std::vector<std::unique_ptr<SpeechSynth>> speech_synths_;
    std::vector<std::unique_ptr<KanjiLog>> kanji_logs_;
};

}  // namespace gekko::yomi
