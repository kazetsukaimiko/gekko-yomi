#pragma once

#include "gekko_yomi/ocr_buffer.hpp"

#include <optional>
#include <string>
#include <vector>

namespace gekko::yomi {

// What core.json is allowed to change. Anything absent keeps the built-in
// default. ocr_engines nullopt means every loaded engine runs. An empty
// vector means none of them run.
struct CoreConfig {
    OcrBufferOptions buffer{};
    std::optional<std::vector<std::string>> ocr_engines;
};

struct ConfigResult {
    CoreConfig config;
    std::vector<std::string> warnings;
    std::string error;
    // True when an optional file is not there. The caller keeps config's defaults.
    bool used_defaults{false};

    bool ok() const { return error.empty(); }
};

// ~/.config/gekko-yomi/core.json from HOME. Empty when HOME is unset.
// XDG_CONFIG_HOME is not consulted; the path is the one the settings window
// will edit later.
std::string default_core_config_path();

// required: a missing path or a missing file is an error.
// Otherwise a missing file returns defaults and used_defaults.
// A file that exists and is not valid JSON is an error either way.
ConfigResult load_core_config(const std::string& path, bool required);

}  // namespace gekko::yomi
