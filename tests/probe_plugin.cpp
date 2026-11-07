#include "gekko_yomi/host.hpp"
#include "gekko_yomi/runtime.hpp"

#include <memory>
#include <string>
#include <utility>

class ProbeOcr final : public gekko::yomi::OcrEngine, public gekko::yomi::Runtime {
public:
    std::string id() const override { return "probe"; }
    std::string description() const override { return "probe ocr"; }
    bool provides_character_boxes() const override { return false; }
    gekko::yomi::Runtime* runtime() override { return this; }

    gekko::yomi::Status start() override {
        if (running_) {
            return gekko::yomi::Status::success();
        }
        running_ = true;
        return gekko::yomi::Status::success();
    }

    void stop() override { running_ = false; }

    gekko::yomi::RuntimeStatus status() override {
        if (running_) {
            return {gekko::yomi::RuntimeState::Running, {}};
        }
        return {gekko::yomi::RuntimeState::Stopped, {}};
    }

    gekko::yomi::Result<gekko::yomi::OcrPass> recognize(const gekko::yomi::Frame&,
                                                        const gekko::yomi::OcrRequest&) override {
        gekko::yomi::OcrPass pass;
        pass.engine_id = "probe";
        pass.model = "probe";
        return gekko::yomi::Result<gekko::yomi::OcrPass>::success(std::move(pass));
    }

private:
    bool running_{false};
};

GEKKO_YOMI_PLUGIN_EXPORT
void gekko_yomi_register(gekko::yomi::Host* host) {
    host->register_ocr({"probe", "probe ocr", false},
                       [](std::string*) -> std::unique_ptr<gekko::yomi::OcrEngine> {
                           return std::make_unique<ProbeOcr>();
                       });
    // Same id. The host keeps the first factory and records a load error.
    host->register_ocr({"probe", "probe ocr again", false},
                       [](std::string*) -> std::unique_ptr<gekko::yomi::OcrEngine> {
                           return std::make_unique<ProbeOcr>();
                       });
    host->register_dictionary({"probe-dict", "probe dictionary that declines"},
                              [](std::string* error) -> std::unique_ptr<gekko::yomi::Dictionary> {
                                  if (error != nullptr) {
                                      *error = "probe dictionary declined";
                                  }
                                  return nullptr;
                              });
}
