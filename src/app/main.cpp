#include "gekko_yomi/registry.hpp"

#include <iostream>

int main() {
    gekko::yomi::Registry registry;
    registry.load();

    if (registry.implementation_count() == 0) {
        std::cerr << "Yeah there's no implementations here buddy\n";
        for (const auto& error : registry.load_errors()) {
            std::cerr << error << '\n';
        }
        return 1;
    }

    std::cerr << "Plugins are loaded, but there is no web UI in this build yet.\n";
    return 1;
}
