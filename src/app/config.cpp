#include "config.hpp"

#include "json_mini.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

namespace gekko::yomi {
namespace {

constexpr unsigned long long kMaxConfigBytes = 1024ull * 1024ull;

bool known_key(const std::vector<std::string_view>& known, std::string_view key) {
    return std::find(known.begin(), known.end(), key) != known.end();
}

void warn_unknown(const Json& object, const std::vector<std::string_view>& known, const std::string& where,
                  std::vector<std::string>* warnings) {
    if (object.type != Json::Type::Object) {
        return;
    }
    for (const auto& entry : object.object) {
        if (!known_key(known, entry.first)) {
            warnings->push_back(where + " has unknown key \"" + entry.first + "\"");
        }
    }
}

bool parse_count(const Json& value, const std::string& name, int* out, std::string* error) {
    if (value.type != Json::Type::Number || !std::isfinite(value.number) || std::floor(value.number) != value.number ||
        value.number < 1 || value.number > 1000000) {
        *error = name + " must be an integer from 1 to 1000000";
        return false;
    }
    *out = static_cast<int>(value.number);
    return true;
}

}  // namespace

std::string default_core_config_path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return {};
    }
    return std::string(home) + "/.config/gekko-yomi/core.json";
}

ConfigResult load_core_config(const std::string& path, bool required) {
    ConfigResult result;
    if (path.empty()) {
        result.error = "config path is empty";
        return result;
    }

    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        result.error = "could not stat " + path + ": " + exists_error.message();
        return result;
    }
    if (!exists) {
        if (required) {
            result.error = "config file not found: " + path;
        } else {
            result.used_defaults = true;
        }
        return result;
    }
    if (!std::filesystem::is_regular_file(path)) {
        result.error = "config path is not a file: " + path;
        return result;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "could not read " + path;
        return result;
    }
    in.seekg(0, std::ios::end);
    const auto length = in.tellg();
    if (length < 0 || static_cast<unsigned long long>(length) > kMaxConfigBytes) {
        result.error = "config file is larger than 1MB: " + path;
        return result;
    }
    in.seekg(0);
    const std::string bytes = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

    std::string parse_error;
    const auto json = json_parse(bytes, &parse_error);
    if (!json) {
        result.error = path + ": " + (parse_error.empty() ? "invalid json" : parse_error);
        return result;
    }
    if (json->type != Json::Type::Object) {
        result.error = path + ": config must be a JSON object";
        return result;
    }
    warn_unknown(*json, {"buffer", "ocr"}, path, &result.warnings);

    if (const Json* buffer = json->field("buffer")) {
        if (buffer->type != Json::Type::Object) {
            result.error = path + ": buffer must be an object";
            return result;
        }
        warn_unknown(*buffer, {"ttl_scans", "positional_invalidation_scans"}, path + " buffer", &result.warnings);
        if (const Json* ttl = buffer->field("ttl_scans")) {
            if (!parse_count(*ttl, path + ": buffer.ttl_scans", &result.config.buffer.ttl_scans, &result.error)) {
                return result;
            }
        }
        if (const Json* positional = buffer->field("positional_invalidation_scans")) {
            if (!parse_count(*positional, path + ": buffer.positional_invalidation_scans",
                             &result.config.buffer.positional_invalidation_scans, &result.error)) {
                return result;
            }
        }
    }

    if (const Json* ocr = json->field("ocr")) {
        if (ocr->type != Json::Type::Object) {
            result.error = path + ": ocr must be an object";
            return result;
        }
        warn_unknown(*ocr, {"engines"}, path + " ocr", &result.warnings);
        if (const Json* engines = ocr->field("engines")) {
            if (engines->type != Json::Type::Array) {
                result.error = path + ": ocr.engines must be an array of strings";
                return result;
            }
            std::vector<std::string> ids;
            ids.reserve(engines->array.size());
            for (const auto& entry : engines->array) {
                if (entry.type != Json::Type::String || entry.text.empty()) {
                    result.error = path + ": ocr.engines must be an array of non-empty strings";
                    return result;
                }
                ids.push_back(entry.text);
            }
            result.config.ocr_engines = std::move(ids);
        }
    }
    return result;
}

std::string save_core_config(const std::string& path, const CoreConfig& config) {
    if (path.empty()) {
        return "config path is empty";
    }
    const std::filesystem::path file(path);
    if (file.has_parent_path()) {
        std::error_code mkdir_error;
        std::filesystem::create_directories(file.parent_path(), mkdir_error);
        if (mkdir_error) {
            return "could not create " + file.parent_path().string() + ": " + mkdir_error.message();
        }
    }

    std::string body = "{\n  \"buffer\": {\n    \"ttl_scans\": ";
    body += std::to_string(config.buffer.ttl_scans);
    body += ",\n    \"positional_invalidation_scans\": ";
    body += std::to_string(config.buffer.positional_invalidation_scans);
    body += "\n  }";
    if (config.ocr_engines) {
        body += ",\n  \"ocr\": {\n    \"engines\": [";
        for (std::size_t i = 0; i < config.ocr_engines->size(); ++i) {
            if (i > 0) {
                body += ", ";
            }
            body += json_quote((*config.ocr_engines)[i]);
        }
        body += "]\n  }";
    }
    body += "\n}\n";

    const std::filesystem::path temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return "could not write " + temporary.string();
        }
        out << body;
        if (!out) {
            return "could not write " + temporary.string();
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, file, rename_error);
    if (rename_error) {
        std::filesystem::remove(temporary);
        return "could not replace " + path + ": " + rename_error.message();
    }
    return {};
}

}  // namespace gekko::yomi
