#include "gekko_yomi/geometry.hpp"
#include "gekko_yomi/registry.hpp"
#include "gekko_yomi/result.hpp"
#include "window_ref.hpp"

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

void test_kwin_ids_and_pixels() {
    const auto uuid = gekko::yomi::kwin::window_uuid_from_runner_id(
        "0_{8ceda663-7dc9-4b1a-be2c-e54c215f4285}");
    CHECK(uuid && *uuid == "{8ceda663-7dc9-4b1a-be2c-e54c215f4285}");
    const auto bare = gekko::yomi::kwin::window_uuid_from_runner_id("{8CEDA663-7DC9-4B1A-BE2C-E54C215F4285}");
    CHECK(bare && *bare == "{8CEDA663-7DC9-4B1A-BE2C-E54C215F4285}");
    CHECK(!gekko::yomi::kwin::window_uuid_from_runner_id("8_a3b75867-c3d8-403b-88b0-26805e8466fe"));
    CHECK(!gekko::yomi::kwin::window_uuid_from_runner_id("0_{not-a-uuid}"));
    CHECK(!gekko::yomi::kwin::window_uuid_from_runner_id(""));

    CHECK(gekko::yomi::kwin::is_capture_window_type(0));
    CHECK(gekko::yomi::kwin::is_capture_window_type(5));
    CHECK(!gekko::yomi::kwin::is_capture_window_type(1));
    CHECK(!gekko::yomi::kwin::is_capture_window_type(2));

    CHECK(near(gekko::yomi::kwin::image_pixels_per_logical(1920, 1280, 1.5), 1.5));
    CHECK(near(gekko::yomi::kwin::image_pixels_per_logical(100, 100, 1.5), 1));
    CHECK(near(gekko::yomi::kwin::image_pixels_per_logical(0, 0, 1.5), 1.5));
    CHECK(near(gekko::yomi::kwin::image_pixels_per_logical(0, 0, 0), 1));

    std::uint8_t rgba[8] = {1, 2, 3, 255, 9, 9, 9, 9};
    CHECK(gekko::yomi::kwin::convert_qimage_to_bgra(18, rgba, 1, 1, 8));
    CHECK(rgba[0] == 3 && rgba[1] == 2 && rgba[2] == 1 && rgba[3] == 255);
    CHECK(rgba[4] == 9 && rgba[7] == 9);
    std::uint8_t bgra[4] = {4, 5, 6, 255};
    CHECK(gekko::yomi::kwin::convert_qimage_to_bgra(6, bgra, 1, 1, 4));
    CHECK(bgra[0] == 4 && bgra[2] == 6);
    CHECK(!gekko::yomi::kwin::convert_qimage_to_bgra(7, bgra, 1, 1, 4));

    gekko::yomi::Frame frame;
    frame.width = 2;
    frame.height = 2;
    frame.stride_bytes = 12;
    frame.pixels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    frame.mapping.window_logical_x = 10;
    frame.mapping.image_pixels_per_logical = 2;
    const auto cropped = gekko::yomi::kwin::crop_frame(frame, {1, 1, 1, 1});
    CHECK(cropped.ok());
    if (cropped.value) {
        CHECK(cropped.value->width == 1);
        CHECK(cropped.value->height == 1);
        CHECK(cropped.value->pixels.size() == 4);
        CHECK(cropped.value->pixels[0] == 17 && cropped.value->pixels[3] == 20);
        CHECK(near(cropped.value->mapping.client_inset_image_x, -1));
        CHECK(near(cropped.value->mapping.client_inset_image_y, -1));
        CHECK(near(cropped.value->mapping.window_logical_x, 10));
    }
    CHECK(!gekko::yomi::kwin::crop_frame(frame, {8, 8, 1, 1}).ok());
}

void test_kwin_session() {
    const char* dir = std::getenv("GEKKO_YOMI_KWIN_DIR");
    CHECK(dir != nullptr);
    if (dir == nullptr) {
        return;
    }
    setenv("GEKKO_YOMI_PLUGINS", dir, 1);
    gekko::yomi::Registry registry;
    registry.load();
    for (const auto& load_error : registry.load_errors()) {
        std::cerr << load_error << '\n';
    }
    CHECK(registry.load_errors().empty());
    gekko::yomi::CaptureSource* kwin = nullptr;
    for (const auto& source : registry.capture_sources()) {
        if (source->id() == "kwin") {
            kwin = source.get();
        }
    }
    CHECK(kwin != nullptr);
    if (kwin == nullptr) {
        return;
    }
    const auto windows = kwin->list_windows();
    if (!windows.ok() && windows.error.find("KWin is not available") != std::string::npos) {
        std::cerr << "SKIP " << windows.error << '\n';
        std::exit(77);
    }
    if (!windows.ok()) {
        std::cerr << windows.error << '\n';
    }
    CHECK(windows.ok());
    if (!windows.value) {
        return;
    }
    const gekko::yomi::WindowInfo* pick = nullptr;
    double area = 1e300;
    for (const auto& window : *windows.value) {
        CHECK(window.id.size() == 38);
        CHECK(!window.id.empty() && window.id.front() == '{' && window.id.back() == '}');
        CHECK(!window.title.empty());
        if (window.minimized || window.geometry.width <= 0 || window.geometry.height <= 0) {
            continue;
        }
        const double this_area = window.geometry.width * window.geometry.height;
        if (this_area < area) {
            area = this_area;
            pick = &window;
        }
    }
    if (pick == nullptr) {
        return;
    }
    const auto frame = kwin->grab_window(pick->id);
    if (!frame.ok()) {
        std::cerr << frame.error << '\n';
    }
    CHECK(frame.ok());
    if (!frame.value) {
        return;
    }
    CHECK(frame.value->width > 0);
    CHECK(frame.value->height > 0);
    CHECK(frame.value->stride_bytes >= frame.value->width * 4);
    CHECK(frame.value->pixels.size() >=
          static_cast<std::size_t>(frame.value->stride_bytes) * static_cast<std::size_t>(frame.value->height));
    // List and grab are separate calls. A one-pixel compositor change still
    // has to land on the width ratio, not on a hardcoded scale of 1.
    const double ratio = static_cast<double>(frame.value->width) / pick->geometry.width;
    CHECK(std::abs(frame.value->mapping.image_pixels_per_logical - ratio) < 1e-3);
    CHECK(std::abs(frame.value->mapping.window_logical_x - pick->geometry.x) < 1);
    CHECK(std::abs(frame.value->mapping.window_logical_y - pick->geometry.y) < 1);
    CHECK(near(frame.value->mapping.client_inset_image_x, 0));
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
        test_kwin_ids_and_pixels();
    } else if (which == "kwin") {
        test_kwin_session();
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
