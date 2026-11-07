#include "gekko_yomi/registry.hpp"

#include "config.hpp"
#include "http_server.hpp"
#include "log.hpp"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int chosen_port() {
    if (const char* env = std::getenv("GEKKO_YOMI_PORT")) {
        return std::atoi(env);
    }
    return -1;
}

std::string join_ids(const std::vector<std::string>& ids) {
    std::string joined;
    for (const auto& id : ids) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += id;
    }
    return joined.empty() ? "none" : joined;
}

}  // namespace

template <typename T>
void log_loaded(const char* kind, const T& items) {
    auto& log = gekko::yomi::logger("plugin");
    if (items.empty()) {
        log.warn("no {} loaded", kind);
        return;
    }
    for (const auto& item : items) {
        log.info("{} {} — {}", kind, item->id(), item->description());
    }
}

int main(int argc, char** argv) {
    gekko::yomi::init_logging();

    std::string config_path;
    CLI::App cli{"Read Japanese from a window and show it with readings."};
    cli.set_version_flag("-V,--version", std::string("gekko-yomi ") + GEKKO_YOMI_VERSION);
    auto* config_option = cli.add_option(
        "--config", config_path, "JSON config file. When omitted, ~/.config/gekko-yomi/core.json is used if it exists");
    config_option->type_name("PATH");
    try {
        cli.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return cli.exit(error);
    }

    gekko::yomi::CoreConfig config;
    const bool explicit_config = config_option->count() > 0;
    if (explicit_config) {
        const auto loaded = gekko::yomi::load_core_config(config_path, true);
        for (const auto& warning : loaded.warnings) {
            gekko::yomi::logger("app").warn("{}", warning);
        }
        if (!loaded.ok()) {
            gekko::yomi::logger("app").error("{}", loaded.error);
            return 1;
        }
        config = loaded.config;
        gekko::yomi::logger("app").info("config {}", config_path);
    } else {
        config_path = gekko::yomi::default_core_config_path();
        if (config_path.empty()) {
            gekko::yomi::logger("app").warn("HOME is unset; using defaults");
        } else {
            const auto loaded = gekko::yomi::load_core_config(config_path, false);
            for (const auto& warning : loaded.warnings) {
                gekko::yomi::logger("app").warn("{}", warning);
            }
            if (!loaded.ok()) {
                gekko::yomi::logger("app").error("{}", loaded.error);
                return 1;
            }
            config = loaded.config;
            if (loaded.used_defaults) {
                gekko::yomi::logger("app").info("no config file at {}; using defaults", config_path);
            } else {
                gekko::yomi::logger("app").info("config {}", config_path);
            }
        }
    }
    gekko::yomi::logger("app").info("buffer ttl_scans={} positional_invalidation_scans={}", config.buffer.ttl_scans,
                                    config.buffer.positional_invalidation_scans);

    gekko::yomi::Registry registry;
    if (config.ocr_engines) {
        registry.limit_ocr_engines(*config.ocr_engines);
    }
    registry.load();
    for (const auto& load_error : registry.load_errors()) {
        gekko::yomi::logger("plugin").error("{}", load_error);
    }
    log_loaded("capture", registry.capture_sources());
    log_loaded("ocr", registry.ocr_engines());
    log_loaded("reading", registry.reading_providers());
    log_loaded("dictionary", registry.dictionaries());
    if (config.ocr_engines) {
        for (const auto& id : *config.ocr_engines) {
            bool found = false;
            for (const auto& engine : registry.ocr_engines()) {
                if (engine->id() == id) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                gekko::yomi::logger("app").warn("ocr engine {} is not loaded", id);
            }
        }
        gekko::yomi::logger("app").info("ocr engines enabled: {}", join_ids(*config.ocr_engines));
    } else {
        gekko::yomi::logger("app").info("ocr engines enabled: all loaded");
    }
    if (const char* python = std::getenv("GEKKO_YOMI_PYTHON")) {
        gekko::yomi::logger("app").info("GEKKO_YOMI_PYTHON={}", python);
    } else {
        gekko::yomi::logger("app").info("GEKKO_YOMI_PYTHON is unset; workers use the venv beside the executable, or python3");
    }

    gekko::yomi::HttpServer server(&registry, config.buffer, config.ocr_engines);
    std::string error;
    bool listening = false;
    const int requested = chosen_port();
    if (requested >= 0) {
        listening = server.listen(requested, &error);
    } else {
        for (int port = 8765; port <= 8775; ++port) {
            error.clear();
            if (server.listen(port, &error)) {
                listening = true;
                break;
            }
        }
    }
    if (!listening) {
        gekko::yomi::logger("app").error("{}", error.empty() ? "could not listen on 0.0.0.0" : error);
        return 1;
    }

    gekko::yomi::logger("app").info("listening on 0.0.0.0:{}", server.port());
    // One line, flushed, so a test can read the URL before the accept loop.
    // Loopback still reaches a 0.0.0.0 socket. Other machines use this host's address.
    std::cout << "http://127.0.0.1:" << server.port() << std::endl;
    server.run();
    return 0;
}
