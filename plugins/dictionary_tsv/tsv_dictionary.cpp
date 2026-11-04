#include "gekko_yomi/host.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gekko::yomi::tsv {
namespace {

constexpr std::uintmax_t kMaxBytes = 256ull * 1024ull * 1024ull;

class TsvDictionary final : public Dictionary {
public:
    explicit TsvDictionary(std::string path) : path_(std::move(path)) {}

    std::string id() const override { return "tsv"; }
    std::string description() const override { return "TSV dictionary at " + path_; }

    Result<std::vector<Gloss>> lookup(std::string_view dictionary_form) override {
        if (!loaded_) {
            load();
        }
        if (!load_error_.empty()) {
            return Result<std::vector<Gloss>>::failure(load_error_);
        }
        const auto found = entries_.find(std::string(dictionary_form));
        if (found == entries_.end()) {
            return Result<std::vector<Gloss>>::success({});
        }
        return Result<std::vector<Gloss>>::success(found->second);
    }

private:
    void load() {
        loaded_ = true;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path_, ec)) {
            load_error_ = "dictionary file not found: " + path_;
            return;
        }
        const auto size = std::filesystem::file_size(path_, ec);
        if (ec) {
            load_error_ = "could not read dictionary file: " + path_;
            return;
        }
        if (size > kMaxBytes) {
            load_error_ = "dictionary file is larger than 256MB: " + path_;
            return;
        }
        std::ifstream in(path_);
        if (!in) {
            load_error_ = "could not open dictionary file: " + path_;
            return;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const auto tab = line.find('\t');
            if (tab == std::string::npos || tab == 0) {
                continue;
            }
            Gloss gloss;
            gloss.dictionary_form = line.substr(0, tab);
            const std::string rest = line.substr(tab + 1);
            const auto tab2 = rest.find('\t');
            std::string english;
            if (tab2 == std::string::npos) {
                english = rest;
            } else {
                gloss.part_of_speech = rest.substr(0, tab2);
                english = rest.substr(tab2 + 1);
            }
            std::size_t at = 0;
            while (at < english.size()) {
                const auto slash = english.find(" / ", at);
                const auto part = english.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
                if (!part.empty()) {
                    gloss.english.push_back(part);
                }
                if (slash == std::string::npos) {
                    break;
                }
                at = slash + 3;
            }
            if (gloss.english.empty()) {
                continue;
            }
            entries_[gloss.dictionary_form].push_back(std::move(gloss));
        }
    }

    std::string path_;
    bool loaded_{false};
    std::string load_error_;
    std::unordered_map<std::string, std::vector<Gloss>> entries_;
};

}  // namespace
}  // namespace gekko::yomi::tsv

GEKKO_YOMI_PLUGIN_EXPORT
void gekko_yomi_register(gekko::yomi::Host* host) {
    const char* path = std::getenv("GEKKO_YOMI_DICTIONARY");
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    host->add(std::make_unique<gekko::yomi::tsv::TsvDictionary>(path));
}
