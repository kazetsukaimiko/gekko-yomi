#include "gekko_yomi/registry.hpp"

#include "http_server.hpp"

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

int main() {
    gekko::yomi::Registry registry;
    registry.load();

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
        std::cerr << (error.empty() ? "could not listen on 127.0.0.1" : error) << '\n';
        return 1;
    }

    // One line, flushed, so a test can read the URL before the accept loop.
    std::cout << "http://127.0.0.1:" << server.port() << std::endl;
    server.run();
    return 0;
}
