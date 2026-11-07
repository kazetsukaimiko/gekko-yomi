#pragma once

#include "gekko_yomi/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gekko::yomi {

class Runtime;

// One remembered lookup. The embedded database behind this is not part of
// the web MVP. The interface is here so the page can call it later
// without a second design.
struct LogEntry {
    std::string id;
    std::int64_t unix_ms{0};
    std::string window_title;
    std::string surface;
    std::string reading;
    std::string gloss;
};

class KanjiLog {
public:
    virtual ~KanjiLog() = default;

    virtual Runtime* runtime() { return nullptr; }

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;

    virtual Status append(const LogEntry& entry) = 0;
    virtual Result<std::vector<LogEntry>> recent(std::size_t limit) = 0;
    virtual Result<std::vector<LogEntry>> search(std::string_view query) = 0;
};

}  // namespace gekko::yomi
