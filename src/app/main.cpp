#include "gekko_yomi/registry.hpp"

#include "http_server.hpp"
#include "log.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int chosen_port() {
    if (const char* env = std::getenv("GEKKO_YOMI_PORT")) {
        return std::atoi(env);
    }
    return -1;
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

int main() {
    gekko::yomi::init_logging();
    gekko::yomi::Registry registry;
    registry.load();
    for (const auto& load_error : registry.load_errors()) {
        gekko::yomi::logger("plugin").error("{}", load_error);
    }
    log_loaded("capture", registry.capture_sources());
    log_loaded("ocr", registry.ocr_engines());
    log_loaded("reading", registry.reading_providers());
    log_loaded("dictionary", registry.dictionaries());
    if (const char* python = std::getenv("GEKKO_YOMI_PYTHON")) {
        gekko::yomi::logger("app").info("GEKKO_YOMI_PYTHON={}", python);
    } else {
        gekko::yomi::logger("app").info("GEKKO_YOMI_PYTHON is unset; workers use the venv beside the executable, or python3");
    }

    gekko::yomi::HttpServer server(&registry);
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
