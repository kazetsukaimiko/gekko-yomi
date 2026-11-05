#include "log.hpp"

// spdlog 1.15+ ships the stderr color sink from this header.
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdlib>
#include <memory>

namespace gekko::yomi {
namespace {

std::shared_ptr<spdlog::sinks::sink> sink;
spdlog::level::level_enum level = spdlog::level::info;

void configure(const std::shared_ptr<spdlog::logger>& created) {
    created->set_pattern("%H:%M:%S.%e %n %^%l%$ %v");
    created->set_level(level);
    created->flush_on(spdlog::level::info);
}

std::shared_ptr<spdlog::logger> make_logger(const char* name) {
    auto created = std::make_shared<spdlog::logger>(name, sink);
    configure(created);
    spdlog::register_logger(created);
    return created;
}

}  // namespace

void init_logging() {
    sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    level = spdlog::level::info;
    if (const char* env = std::getenv("GEKKO_YOMI_LOG")) {
        if (env[0] != '\0') {
            level = spdlog::level::from_str(env);
        }
    }
    auto app = make_logger("app");
    make_logger("plugin");
    make_logger("http");
    make_logger("capture");
    make_logger("ocr");
    make_logger("reading");
    make_logger("dictionary");
    spdlog::set_default_logger(std::move(app));
}

spdlog::logger& logger(const char* name) {
    if (auto found = spdlog::get(name)) {
        return *found;
    }
    if (!sink) {
        sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    }
    return *make_logger(name);
}

}  // namespace gekko::yomi
