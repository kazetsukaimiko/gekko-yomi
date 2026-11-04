#pragma once

#include "gekko_yomi/host.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

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

private:
    void load_directory(const std::string& directory);

    std::vector<void*> handles_;
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
