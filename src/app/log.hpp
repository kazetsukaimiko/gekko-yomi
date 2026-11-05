#pragma once

#include <spdlog/spdlog.h>

namespace gekko::yomi {

// stderr only. The process prints its URL on stdout, and a test reads that line.
// A line is time, logger name, level, message.
// GEKKO_YOMI_LOG selects the level for every logger: trace, debug, info, warn, error, critical, off.
void init_logging();

// Named loggers: app, plugin, http, capture, ocr, reading, dictionary.
spdlog::logger& logger(const char* name);

}  // namespace gekko::yomi
