#include "gekko_yomi/registry.hpp"

#include <dlfcn.h>

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

Registry::Registry() = default;

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
    for (void* handle : handles_) {
        ::dlclose(handle);
    }
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
        auto* register_fn = reinterpret_cast<RegisterFn>(symbol);
        register_fn(this);
        handles_.push_back(handle);
    }
}

std::size_t Registry::implementation_count() const {
    return capture_sources_.size() + ocr_engines_.size() + language_models_.size() +
           style_samplers_.size() + reading_providers_.size() + dictionaries_.size() +
           presenters_.size() + overlays_.size() + speech_synths_.size() + kanji_logs_.size();
}

void Registry::add(std::unique_ptr<CaptureSource> source) { capture_sources_.push_back(std::move(source)); }
void Registry::add(std::unique_ptr<OcrEngine> engine) { ocr_engines_.push_back(std::move(engine)); }
void Registry::add(std::unique_ptr<LanguageModel> model) { language_models_.push_back(std::move(model)); }
void Registry::add(std::unique_ptr<StyleSampler> sampler) { style_samplers_.push_back(std::move(sampler)); }
void Registry::add(std::unique_ptr<ReadingProvider> provider) {
    reading_providers_.push_back(std::move(provider));
}
void Registry::add(std::unique_ptr<Dictionary> dictionary) { dictionaries_.push_back(std::move(dictionary)); }
void Registry::add(std::unique_ptr<Presenter> presenter) { presenters_.push_back(std::move(presenter)); }
void Registry::add(std::unique_ptr<Overlay> overlay) { overlays_.push_back(std::move(overlay)); }
void Registry::add(std::unique_ptr<SpeechSynth> speech) { speech_synths_.push_back(std::move(speech)); }
void Registry::add(std::unique_ptr<KanjiLog> log) { kanji_logs_.push_back(std::move(log)); }

}  // namespace gekko::yomi
