#include "gekko_yomi/host.hpp"

#include <memory>
#include <string>
#include <utility>

namespace {

class FakeOcr final : public gekko::yomi::OcrEngine {
public:
    std::string id() const override { return "fake-ocr"; }
    std::string description() const override { return "fixture ocr"; }
    bool provides_character_boxes() const override { return true; }

    gekko::yomi::Result<gekko::yomi::OcrPass> recognize(const gekko::yomi::Frame& frame,
                                                        const gekko::yomi::OcrRequest&) override {
        gekko::yomi::OcrPass pass;
        pass.engine_id = "fake-ocr";
        pass.model = "fixture";
        gekko::yomi::TextRun run;
        run.text = "漢字";
        run.box = {0, 0, static_cast<double>(frame.width), static_cast<double>(frame.height)};
        gekko::yomi::CharBox first;
        first.text = "漢";
        first.box = {0, 0, 1, 1};
        gekko::yomi::CharBox second;
        second.text = "字";
        second.box = {1, 0, 1, 1};
        run.characters.push_back(std::move(first));
        run.characters.push_back(std::move(second));
        pass.runs.push_back(std::move(run));
        return gekko::yomi::Result<gekko::yomi::OcrPass>::success(std::move(pass));
    }
};

}  // namespace

GEKKO_YOMI_PLUGIN_EXPORT
void gekko_yomi_register(gekko::yomi::Host* host) {
    host->add(std::make_unique<FakeOcr>());
}
