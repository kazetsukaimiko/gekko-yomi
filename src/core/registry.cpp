#include "gekko_yomi/registry.hpp"

#include "gekko_yomi/runtime.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <utility>

namespace gekko::yomi {
namespace {

using RegisterFn = void (*)(Host*);

std::filesystem::path executable_directory() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) {
        return {};
    }
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path();
}

std::vector<std::string> split_plugin_dirs(const char* value) {
    std::vector<std::string> dirs;
    std::string rest(value == nullptr ? "" : value);
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        const auto piece = rest.substr(0, colon);
        if (!piece.empty()) {
            dirs.push_back(piece);
        }
        if (colon == std::string::npos) {
            break;
        }
        rest.erase(0, colon + 1);
    }
    return dirs;
}

std::vector<std::string> plugin_directories() {
    // An explicit list replaces the defaults, including when it names one
    // directory. Tests use that to load a fixture plugin and nothing else.
    if (const char* env = std::getenv("GEKKO_YOMI_PLUGINS")) {
        return split_plugin_dirs(env);
    }
    std::vector<std::string> dirs;

    const auto exe_dir = executable_directory();
    if (!exe_dir.empty()) {
        dirs.push_back((exe_dir / "plugins").string());
        dirs.push_back((exe_dir / ".." / "lib" / "gekko-yomi" / "plugins").lexically_normal().string());
    }
    return dirs;
}

}  // namespace

struct Registry::Module {
    template <typename Interface, typename Factory>
    struct Slot {
        std::string id;
        std::string description;
        bool character_boxes{false};
        Factory create{nullptr};
        Interface* instance{nullptr};
    };

    Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&& other) noexcept { *this = std::move(other); }
    Module& operator=(Module&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (handle != nullptr) {
            ::dlclose(handle);
        }
        handle = other.handle;
        other.handle = nullptr;
        path = std::move(other.path);
        capture = std::move(other.capture);
        ocr = std::move(other.ocr);
        language = std::move(other.language);
        style = std::move(other.style);
        reading = std::move(other.reading);
        dictionary = std::move(other.dictionary);
        presenter = std::move(other.presenter);
        overlay = std::move(other.overlay);
        speech = std::move(other.speech);
        kanji = std::move(other.kanji);
        adopted_capture = std::move(other.adopted_capture);
        adopted_ocr = std::move(other.adopted_ocr);
        adopted_language = std::move(other.adopted_language);
        adopted_style = std::move(other.adopted_style);
        adopted_reading = std::move(other.adopted_reading);
        adopted_dictionary = std::move(other.adopted_dictionary);
        adopted_presenter = std::move(other.adopted_presenter);
        adopted_overlay = std::move(other.adopted_overlay);
        adopted_speech = std::move(other.adopted_speech);
        adopted_kanji = std::move(other.adopted_kanji);
        return *this;
    }
    ~Module() {
        if (handle != nullptr) {
            ::dlclose(handle);
        }
    }

    void* handle{nullptr};
    std::string path;
    std::vector<Slot<CaptureSource, CaptureFactory>> capture;
    std::vector<Slot<OcrEngine, OcrFactory>> ocr;
    std::vector<Slot<LanguageModel, LanguageModelFactory>> language;
    std::vector<Slot<StyleSampler, StyleSamplerFactory>> style;
    std::vector<Slot<ReadingProvider, ReadingFactory>> reading;
    std::vector<Slot<Dictionary, DictionaryFactory>> dictionary;
    std::vector<Slot<Presenter, PresenterFactory>> presenter;
    std::vector<Slot<Overlay, OverlayFactory>> overlay;
    std::vector<Slot<SpeechSynth, SpeechFactory>> speech;
    std::vector<Slot<KanjiLog, KanjiLogFactory>> kanji;
    std::vector<CaptureSource*> adopted_capture;
    std::vector<OcrEngine*> adopted_ocr;
    std::vector<LanguageModel*> adopted_language;
    std::vector<StyleSampler*> adopted_style;
    std::vector<ReadingProvider*> adopted_reading;
    std::vector<Dictionary*> adopted_dictionary;
    std::vector<Presenter*> adopted_presenter;
    std::vector<Overlay*> adopted_overlay;
    std::vector<SpeechSynth*> adopted_speech;
    std::vector<KanjiLog*> adopted_kanji;
};

struct Registry::Modules {
    std::vector<Module> items;
};

Registry::Registry() : modules_(std::make_unique<Modules>()) {}

Registry::~Registry() {
    capture_sources_.clear();
    ocr_engines_.clear();
    language_models_.clear();
    style_samplers_.clear();
    reading_providers_.clear();
    dictionaries_.clear();
    presenters_.clear();
    overlays_.clear();
    speech_synths_.clear();
    kanji_logs_.clear();
    modules_.reset();
}

void Registry::load() {
    for (const auto& dir : plugin_directories()) {
        load_directory(dir);
    }
}

void Registry::load_directory(const std::string& directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            load_errors_.push_back(directory + ": " + ec.message());
            return;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".so") {
            continue;
        }
        const std::string path = entry.path().string();
        const bool loaded = std::any_of(modules_->items.begin(), modules_->items.end(),
                                         [&](const Module& module) { return module.path == path; });
        if (loaded) {
            continue;
        }
        void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            const char* why = ::dlerror();
            load_errors_.push_back(path + ": " + (why != nullptr ? why : "dlopen failed"));
            continue;
        }
        ::dlerror();
        auto* symbol = ::dlsym(handle, "gekko_yomi_register");
        const char* why = ::dlerror();
        if (symbol == nullptr || why != nullptr) {
            load_errors_.push_back(path + ": missing gekko_yomi_register");
            ::dlclose(handle);
            continue;
        }
        modules_->items.push_back(Module{});
        Module& module = modules_->items.back();
        module.handle = handle;
        module.path = path;
        loading_ = &module;
        auto* register_fn = reinterpret_cast<RegisterFn>(symbol);
        register_fn(this);
        loading_ = nullptr;
        instantiate(module);
    }
}

std::size_t Registry::implementation_count() const {
    return capture_sources_.size() + ocr_engines_.size() + language_models_.size() +
           style_samplers_.size() + reading_providers_.size() + dictionaries_.size() +
           presenters_.size() + overlays_.size() + speech_synths_.size() + kanji_logs_.size();
}

namespace {

std::string copy_text(const char* text) { return text == nullptr ? std::string() : std::string(text); }

template <typename Interface>
bool live_id(const std::vector<std::unique_ptr<Interface>>& live, const std::string& id) {
    return std::any_of(live.begin(), live.end(), [&](const std::unique_ptr<Interface>& item) {
        return item && item->id() == id;
    });
}

template <typename Interface>
void destroy_instance(std::vector<std::unique_ptr<Interface>>* live, Interface* instance) {
    if (instance == nullptr || live == nullptr) {
        return;
    }
    if (Runtime* runtime = instance->runtime()) {
        runtime->stop();
    }
    live->erase(std::remove_if(live->begin(), live->end(),
                               [&](const std::unique_ptr<Interface>& item) { return item.get() == instance; }),
                live->end());
}

}  // namespace

bool Registry::id_taken(const std::string& id) const {
    if (live_id(capture_sources_, id) || live_id(ocr_engines_, id) || live_id(language_models_, id) ||
        live_id(style_samplers_, id) || live_id(reading_providers_, id) || live_id(dictionaries_, id) ||
        live_id(presenters_, id) || live_id(overlays_, id) || live_id(speech_synths_, id) || live_id(kanji_logs_, id)) {
        return true;
    }
    const auto slots_have = [&](const auto& slots) {
        return std::any_of(slots.begin(), slots.end(), [&](const auto& slot) { return slot.id == id; });
    };
    for (const Module& module : modules_->items) {
        if (slots_have(module.capture) || slots_have(module.ocr) || slots_have(module.language) ||
            slots_have(module.style) || slots_have(module.reading) || slots_have(module.dictionary) ||
            slots_have(module.presenter) || slots_have(module.overlay) || slots_have(module.speech) ||
            slots_have(module.kanji)) {
            return true;
        }
    }
    return false;
}

void Registry::limit_ocr_engines(std::vector<std::string> ids) { ocr_limit_ = std::move(ids); }

void Registry::instantiate(Module& module) {
    auto create = [&](auto* slots, auto* live, bool limited) {
        for (auto& slot : *slots) {
            if (slot.create == nullptr || slot.instance != nullptr) {
                continue;
            }
            if (limited && ocr_limit_ &&
                std::find(ocr_limit_->begin(), ocr_limit_->end(), slot.id) == ocr_limit_->end()) {
                continue;
            }
            std::string error;
            auto made = slot.create(&error);
            if (!made) {
                load_errors_.push_back(module.path + ": " + (error.empty() ? slot.id + " declined" : error));
                continue;
            }
            if (made->id() != slot.id) {
                load_errors_.push_back(module.path + ": " + slot.id + " factory returned " + made->id());
                continue;
            }
            slot.instance = made.get();
            live->push_back(std::move(made));
        }
    };
    create(&module.capture, &capture_sources_, false);
    create(&module.ocr, &ocr_engines_, true);
    create(&module.language, &language_models_, false);
    create(&module.style, &style_samplers_, false);
    create(&module.reading, &reading_providers_, false);
    create(&module.dictionary, &dictionaries_, false);
    create(&module.presenter, &presenters_, false);
    create(&module.overlay, &overlays_, false);
    create(&module.speech, &speech_synths_, false);
    create(&module.kanji, &kanji_logs_, false);
    for (const auto& slot : module.ocr) {
        if (slot.instance != nullptr && slot.instance->provides_character_boxes() != slot.character_boxes) {
            load_errors_.push_back(module.path + ": " + slot.id + " character_boxes does not match the engine");
        }
    }
}

void Registry::drop_module(Module& module) {
    auto drop_slots = [](auto* slots, auto* live) {
        for (auto& slot : *slots) {
            destroy_instance(live, slot.instance);
            slot.instance = nullptr;
            slot.create = nullptr;
        }
    };
    auto drop_adopted = [](auto* adopted, auto* live) {
        for (auto* instance : *adopted) {
            destroy_instance(live, instance);
        }
        adopted->clear();
    };
    drop_slots(&module.capture, &capture_sources_);
    drop_slots(&module.ocr, &ocr_engines_);
    drop_slots(&module.language, &language_models_);
    drop_slots(&module.style, &style_samplers_);
    drop_slots(&module.reading, &reading_providers_);
    drop_slots(&module.dictionary, &dictionaries_);
    drop_slots(&module.presenter, &presenters_);
    drop_slots(&module.overlay, &overlays_);
    drop_slots(&module.speech, &speech_synths_);
    drop_slots(&module.kanji, &kanji_logs_);
    drop_adopted(&module.adopted_capture, &capture_sources_);
    drop_adopted(&module.adopted_ocr, &ocr_engines_);
    drop_adopted(&module.adopted_language, &language_models_);
    drop_adopted(&module.adopted_style, &style_samplers_);
    drop_adopted(&module.adopted_reading, &reading_providers_);
    drop_adopted(&module.adopted_dictionary, &dictionaries_);
    drop_adopted(&module.adopted_presenter, &presenters_);
    drop_adopted(&module.adopted_overlay, &overlays_);
    drop_adopted(&module.adopted_speech, &speech_synths_);
    drop_adopted(&module.adopted_kanji, &kanji_logs_);
}

std::vector<std::string> Registry::registered_ocr_ids() const {
    std::vector<std::string> ids;
    for (const Module& module : modules_->items) {
        for (const auto& slot : module.ocr) {
            if (!slot.id.empty() && std::find(ids.begin(), ids.end(), slot.id) == ids.end()) {
                ids.push_back(slot.id);
            }
        }
    }
    for (const auto& engine : ocr_engines_) {
        if (engine && std::find(ids.begin(), ids.end(), engine->id()) == ids.end()) {
            ids.push_back(engine->id());
        }
    }
    return ids;
}

std::vector<std::string> Registry::module_paths() const {
    std::vector<std::string> paths;
    paths.reserve(modules_->items.size());
    for (const Module& module : modules_->items) {
        paths.push_back(module.path);
    }
    return paths;
}

Status Registry::unload(const std::string& path) {
    if (scan_depth_ > 0) {
        return Status::failure("cannot unload a plugin while a scan is running");
    }
    const auto found = std::find_if(modules_->items.begin(), modules_->items.end(),
                                    [&](const Module& module) { return module.path == path; });
    if (found == modules_->items.end()) {
        return Status::failure("plugin is not loaded: " + path);
    }
    drop_module(*found);
    modules_->items.erase(found);
    return Status::success();
}

Status Registry::release_instance(const std::string& id) {
    if (scan_depth_ > 0) {
        return Status::failure("cannot change a plugin while a scan is running");
    }
    bool found = false;
    auto release = [&](auto* slots, auto* live) {
        for (auto& slot : *slots) {
            if (slot.id == id && slot.instance != nullptr) {
                destroy_instance(live, slot.instance);
                slot.instance = nullptr;
                found = true;
            }
        }
    };
    for (Module& module : modules_->items) {
        release(&module.capture, &capture_sources_);
        release(&module.ocr, &ocr_engines_);
        release(&module.language, &language_models_);
        release(&module.style, &style_samplers_);
        release(&module.reading, &reading_providers_);
        release(&module.dictionary, &dictionaries_);
        release(&module.presenter, &presenters_);
        release(&module.overlay, &overlays_);
        release(&module.speech, &speech_synths_);
        release(&module.kanji, &kanji_logs_);
    }
    if (!found) {
        return Status::failure("plugin instance is not live: " + id);
    }
    return Status::success();
}

Status Registry::create_instance(const std::string& id) {
    if (scan_depth_ > 0) {
        return Status::failure("cannot change a plugin while a scan is running");
    }
    Status result = Status::failure("plugin is not registered: " + id);
    bool done = false;
    auto create = [&](auto* slots, auto* live) {
        if (done) {
            return;
        }
        for (auto& slot : *slots) {
            if (slot.id != id) {
                continue;
            }
            done = true;
            if (slot.create == nullptr) {
                result = Status::failure("plugin factory is gone: " + id);
                return;
            }
            if (slot.instance != nullptr) {
                result = Status::failure("plugin instance already exists: " + id);
                return;
            }
            std::string error;
            auto made = slot.create(&error);
            if (!made) {
                result = Status::failure(error.empty() ? id + " declined" : error);
                return;
            }
            if (made->id() != slot.id) {
                result = Status::failure(slot.id + " factory returned " + made->id());
                return;
            }
            slot.instance = made.get();
            live->push_back(std::move(made));
            result = Status::success();
            return;
        }
    };
    for (Module& module : modules_->items) {
        create(&module.capture, &capture_sources_);
        create(&module.ocr, &ocr_engines_);
        create(&module.language, &language_models_);
        create(&module.style, &style_samplers_);
        create(&module.reading, &reading_providers_);
        create(&module.dictionary, &dictionaries_);
        create(&module.presenter, &presenters_);
        create(&module.overlay, &overlays_);
        create(&module.speech, &speech_synths_);
        create(&module.kanji, &kanji_logs_);
    }
    for (const Module& module : modules_->items) {
        for (const auto& slot : module.ocr) {
            if (slot.instance != nullptr && slot.id == id &&
                slot.instance->provides_character_boxes() != slot.character_boxes) {
                load_errors_.push_back(module.path + ": " + slot.id + " character_boxes does not match the engine");
            }
        }
    }
    return result;
}

void Registry::begin_scan() { ++scan_depth_; }

void Registry::end_scan() {
    if (scan_depth_ > 0) {
        --scan_depth_;
    }
}

void Registry::add(std::unique_ptr<CaptureSource> source) {
    if (loading_ != nullptr && source) {
        loading_->adopted_capture.push_back(source.get());
    }
    capture_sources_.push_back(std::move(source));
}
void Registry::add(std::unique_ptr<OcrEngine> engine) {
    if (loading_ != nullptr && engine) {
        loading_->adopted_ocr.push_back(engine.get());
    }
    ocr_engines_.push_back(std::move(engine));
}
void Registry::add(std::unique_ptr<LanguageModel> model) {
    if (loading_ != nullptr && model) {
        loading_->adopted_language.push_back(model.get());
    }
    language_models_.push_back(std::move(model));
}
void Registry::add(std::unique_ptr<StyleSampler> sampler) {
    if (loading_ != nullptr && sampler) {
        loading_->adopted_style.push_back(sampler.get());
    }
    style_samplers_.push_back(std::move(sampler));
}
void Registry::add(std::unique_ptr<ReadingProvider> provider) {
    if (loading_ != nullptr && provider) {
        loading_->adopted_reading.push_back(provider.get());
    }
    reading_providers_.push_back(std::move(provider));
}
void Registry::add(std::unique_ptr<Dictionary> dictionary) {
    if (loading_ != nullptr && dictionary) {
        loading_->adopted_dictionary.push_back(dictionary.get());
    }
    dictionaries_.push_back(std::move(dictionary));
}
void Registry::add(std::unique_ptr<Presenter> presenter) {
    if (loading_ != nullptr && presenter) {
        loading_->adopted_presenter.push_back(presenter.get());
    }
    presenters_.push_back(std::move(presenter));
}
void Registry::add(std::unique_ptr<Overlay> overlay) {
    if (loading_ != nullptr && overlay) {
        loading_->adopted_overlay.push_back(overlay.get());
    }
    overlays_.push_back(std::move(overlay));
}
void Registry::add(std::unique_ptr<SpeechSynth> speech) {
    if (loading_ != nullptr && speech) {
        loading_->adopted_speech.push_back(speech.get());
    }
    speech_synths_.push_back(std::move(speech));
}
void Registry::add(std::unique_ptr<KanjiLog> log) {
    if (loading_ != nullptr && log) {
        loading_->adopted_kanji.push_back(log.get());
    }
    kanji_logs_.push_back(std::move(log));
}

bool Registry::prepare_id(const char* id, const char* kind) {
    const char* label = kind == nullptr ? "plugin" : kind;
    if (loading_ == nullptr) {
        load_errors_.push_back(std::string(label) + " factory was registered outside plugin load");
        return false;
    }
    const std::string copied = copy_text(id);
    if (copied.empty()) {
        load_errors_.push_back(loading_->path + ": " + label + " factory is missing an id");
        return false;
    }
    if (id_taken(copied)) {
        load_errors_.push_back(loading_->path + ": duplicate plugin id " + copied);
        return false;
    }
    return true;
}

template <typename Slot, typename Factory>
void Registry::register_named(std::vector<Slot>* slots, PluginDescriptor descriptor, Factory create, const char* kind) {
    if (create == nullptr) {
        load_errors_.push_back(std::string(kind == nullptr ? "plugin" : kind) + " factory is missing");
        return;
    }
    if (!prepare_id(descriptor.id, kind)) {
        return;
    }
    Slot slot;
    slot.id = copy_text(descriptor.id);
    slot.description = copy_text(descriptor.description);
    slot.create = create;
    slots->push_back(std::move(slot));
}

void Registry::register_capture(PluginDescriptor descriptor, CaptureFactory create) {
    register_named(loading_ == nullptr ? nullptr : &loading_->capture, descriptor, create, "capture");
}
void Registry::register_ocr(OcrDescriptor descriptor, OcrFactory create) {
    if (create == nullptr) {
        load_errors_.push_back("ocr factory is missing");
        return;
    }
    if (!prepare_id(descriptor.id, "ocr")) {
        return;
    }
    Module::Slot<OcrEngine, OcrFactory> slot;
    slot.id = copy_text(descriptor.id);
    slot.description = copy_text(descriptor.description);
    slot.character_boxes = descriptor.character_boxes;
    slot.create = create;
    loading_->ocr.push_back(std::move(slot));
}
void Registry::register_language_model(PluginDescriptor descriptor, LanguageModelFactory create) {
    register_named(loading_ == nullptr ? nullptr : &loading_->language, descriptor, create, "language model");
}
void Registry::register_style_sampler(PluginDescriptor descriptor, StyleSamplerFactory create) {
    register_named(loading_ == nullptr ? nullptr : &loading_->style, descriptor, create, "style sampler");
}
void Registry::register_reading(PluginDescriptor descriptor, ReadingFactory create) {
    register_named(loading_ == nullptr ? nullptr : &loading_->reading, descriptor, create, "reading");
}
void Registry::register_dictionary(PluginDescriptor descriptor, DictionaryFactory create) {
    register_named(loading_ == nullptr ? nullptr : &loading_->dictionary, descriptor, create, "dictionary");
}
void Registry::register_presenter(PluginDescriptor descriptor, PresenterFactory create) {
    register_named(loading_ == nullptr ? nullptr : &loading_->presenter, descriptor, create, "presenter");
}
void Registry::register_overlay(PluginDescriptor descriptor, OverlayFactory create) {
    register_named(loading_ == nullptr ? nullptr : &loading_->overlay, descriptor, create, "overlay");
}
void Registry::register_speech(PluginDescriptor descriptor, SpeechFactory create) {
    register_named(loading_ == nullptr ? nullptr : &loading_->speech, descriptor, create, "speech");
}
void Registry::register_kanji_log(PluginDescriptor descriptor, KanjiLogFactory create) {
    register_named(loading_ == nullptr ? nullptr : &loading_->kanji, descriptor, create, "kanji log");
}

}  // namespace gekko::yomi
