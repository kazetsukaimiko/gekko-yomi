#include "gekko_yomi/host.hpp"

#include <memory>
#include <string>
#include <utility>

namespace {

class FakeCapture final : public gekko::yomi::CaptureSource {
public:
    std::string id() const override { return "fake"; }
    std::string description() const override { return "fixture capture"; }

    gekko::yomi::Result<std::vector<gekko::yomi::WindowInfo>> list_windows() override {
        gekko::yomi::WindowInfo window;
        window.id = "window-1";
        window.title = "Fixture";
        window.app_id = "fixture";
        window.geometry = {0, 0, 10, 20};
        return gekko::yomi::Result<std::vector<gekko::yomi::WindowInfo>>::success(
            std::vector<gekko::yomi::WindowInfo>{std::move(window)});
    }

    gekko::yomi::Result<gekko::yomi::Frame> grab_window(std::string_view) override {
        gekko::yomi::Frame frame;
        frame.width = 2;
        frame.height = 1;
        frame.stride_bytes = 8;
        frame.pixels = {1, 2, 3, 255, 4, 5, 6, 255};
        return gekko::yomi::Result<gekko::yomi::Frame>::success(std::move(frame));
    }

    gekko::yomi::Result<gekko::yomi::Frame> grab_region(std::string_view, const gekko::yomi::Rect&) override {
        return grab_window({});
    }
};

}  // namespace

GEKKO_YOMI_PLUGIN_EXPORT
void gekko_yomi_register(gekko::yomi::Host* host) {
    host->add(std::make_unique<FakeCapture>());
}
