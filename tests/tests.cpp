#include "gekko_yomi/geometry.hpp"
#include "gekko_yomi/registry.hpp"
#include "gekko_yomi/result.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* expr, int line) {
    if (!ok) {
        std::cerr << "FAIL " << line << ": " << expr << '\n';
        ++g_failures;
    }
}

#define CHECK(expr) check(static_cast<bool>(expr), #expr, __LINE__)

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

void test_geometry() {
    gekko::yomi::FrameMapping mapping;
    mapping.window_logical_x = 100;
    mapping.window_logical_y = 40;
    mapping.image_pixels_per_logical = 2;
    mapping.client_inset_image_x = 4;
    mapping.client_inset_image_y = 6;

    const gekko::yomi::Rect image{4, 6, 20, 10};
    const auto overlay = gekko::yomi::image_rect_to_overlay(image, mapping);
    CHECK(near(overlay.x, 100));
    CHECK(near(overlay.y, 40));
    CHECK(near(overlay.width, 10));
    CHECK(near(overlay.height, 5));

    gekko::yomi::FrameMapping identity;
    identity.window_logical_x = 3;
    identity.window_logical_y = 4;
    const auto same = gekko::yomi::image_rect_to_overlay({1, 2, 3, 4}, identity);
    CHECK(near(same.x, 4));
    CHECK(near(same.y, 6));
    CHECK(near(same.width, 3));
    CHECK(near(same.height, 4));

    gekko::yomi::FrameMapping zero = identity;
    zero.image_pixels_per_logical = 0;
    const auto fallback = gekko::yomi::image_rect_to_overlay({8, 0, 2, 2}, zero);
    CHECK(near(fallback.x, 11));
    CHECK(near(fallback.width, 2));
}

void test_result() {
    const auto ok = gekko::yomi::Result<int>::success(7);
    CHECK(ok.ok());
    CHECK(ok.value.has_value() && *ok.value == 7);

    const auto bad = gekko::yomi::Result<int>::failure("nope");
    CHECK(!bad.ok());
    CHECK(!bad.value.has_value());
    CHECK(bad.error == "nope");

    CHECK(gekko::yomi::Status::success().ok());
    CHECK(!gekko::yomi::Status::failure("x").ok());
}

void test_registry_loads_fake() {
    const char* dir = std::getenv("GEKKO_YOMI_FAKE_DIR");
    CHECK(dir != nullptr);
    if (dir == nullptr) {
        return;
    }
    setenv("GEKKO_YOMI_PLUGINS", dir, 1);
    gekko::yomi::Registry registry;
    registry.load();
    CHECK(registry.load_errors().empty());
    CHECK(registry.capture_sources().size() == 1);
    CHECK(registry.ocr_engines().empty());
    CHECK(registry.implementation_count() == 1);
    if (!registry.capture_sources().empty()) {
        CHECK(registry.capture_sources().front()->id() == "fake");
        const auto windows = registry.capture_sources().front()->list_windows();
        CHECK(windows.ok());
        CHECK(windows.value && windows.value->size() == 1);
        CHECK(windows.value && windows.value->front().title == "Fixture");
    }
}

void test_registry_reports_missing_symbol() {
    const char* dir = std::getenv("GEKKO_YOMI_EMPTY_DIR");
    CHECK(dir != nullptr);
    if (dir == nullptr) {
        return;
    }
    setenv("GEKKO_YOMI_PLUGINS", dir, 1);
    gekko::yomi::Registry registry;
    registry.load();
    CHECK(registry.implementation_count() == 0);
    CHECK(registry.load_errors().size() == 1);
    if (!registry.load_errors().empty()) {
        CHECK(registry.load_errors().front().find("gekko_yomi_register") != std::string::npos);
    }
}

void test_explicit_plugin_path_replaces_defaults() {
    const auto empty = std::filesystem::temp_directory_path() / "gekko-yomi-empty-plugins";
    std::filesystem::create_directories(empty);
    setenv("GEKKO_YOMI_PLUGINS", empty.c_str(), 1);
    gekko::yomi::Registry registry;
    registry.load();
    CHECK(registry.implementation_count() == 0);
    CHECK(registry.load_errors().empty());
}

std::string slurp_fd(int fd) {
    std::string out;
    char buf[256];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

void test_app_without_plugins() {
    const char* app = std::getenv("GEKKO_YOMI_APP");
    CHECK(app != nullptr);
    if (app == nullptr) {
        return;
    }
    const auto empty = std::filesystem::temp_directory_path() / "gekko-yomi-empty-plugins";
    std::filesystem::create_directories(empty);

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        setenv("GEKKO_YOMI_PLUGINS", empty.c_str(), 1);
        execl(app, app, static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipefd[1]);
    const std::string err = slurp_fd(pipefd[0]);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(err.find("Yeah there's no implementations here buddy") != std::string::npos);
}

void test_wayland_runs_same_geometry() {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr) {
        std::cerr << "SKIP no WAYLAND_DISPLAY\n";
        std::exit(77);
    }
    // Same assertions as the unit geometry test. The fixture is the session:
    // this process is running on a Wayland display. The numbers do not change.
    test_geometry();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "core";
    if (which == "wayland") {
        test_wayland_runs_same_geometry();
    } else if (which == "core") {
        test_geometry();
        test_result();
        test_registry_loads_fake();
        test_registry_reports_missing_symbol();
        test_explicit_plugin_path_replaces_defaults();
        test_app_without_plugins();
    } else {
        std::cerr << "unknown suite: " << which << '\n';
        return 2;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " failed\n";
        return 1;
    }
    return 0;
}
