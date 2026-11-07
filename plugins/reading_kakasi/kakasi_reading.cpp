#include "text_worker.hpp"

#include "gekko_yomi/host.hpp"
#include "gekko_yomi/runtime.hpp"
#include "json_mini.hpp"

#include <memory>
#include <string>
#include <utility>

namespace gekko::yomi::kakasi {

class KakasiReading final : public ReadingProvider, public Runtime {
public:
    ~KakasiReading() override { stop(); }

    std::string id() const override { return "kakasi"; }
    Runtime* runtime() override { return this; }

    Status start() override {
        if (worker_.running()) {
            return Status::success();
        }
        std::string error;
        if (!worker_.start(default_python(), default_worker_script(), &error)) {
            last_error_ = error;
            return Status::failure(error);
        }
        last_error_.clear();
        return Status::success();
    }

    void stop() override {
        worker_.close();
        last_error_.clear();
    }

    RuntimeStatus status() override {
        if (worker_.running()) {
            return RuntimeStatus{RuntimeState::Running, {}};
        }
        if (!last_error_.empty()) {
            return RuntimeStatus{RuntimeState::Failed, last_error_};
        }
        return RuntimeStatus{RuntimeState::Stopped, {}};
    }
    std::string description() const override {
        return "pykakasi worker. Readings are kana for the surface; the dictionary form is that surface.";
    }

    Result<std::vector<Morpheme>> analyze(std::string_view surface) override {
        if (surface.empty()) {
            return Result<std::vector<Morpheme>>::success({});
        }
        if (const Status began = start(); !began.ok()) {
            return Result<std::vector<Morpheme>>::failure(began.error);
        }
        std::string error;
        const std::string request = std::string("{\"text\":") + json_quote(surface) + "}";
        const auto reply = worker_.request(request, 30000, &error);
        if (!reply) {
            last_error_ = error;
            worker_.close();
            return Result<std::vector<Morpheme>>::failure(error);
        }
        const auto json = json_parse(*reply, &error);
        if (!json) {
            worker_.close();
            return Result<std::vector<Morpheme>>::failure("reading worker returned invalid json: " + error);
        }
        if (const Json* worker_error = json->field("error");
            worker_error != nullptr && worker_error->type == Json::Type::String && !worker_error->text.empty()) {
            // The import path exits after this line. Closing here also covers
            // the moment before waitpid can see that exit, so the next
            // analyze() starts a new process instead of writing to a dead pipe.
            last_error_ = worker_error->text;
            worker_.close();
            return Result<std::vector<Morpheme>>::failure(worker_error->text);
        }
        const Json* list = json->field("morphemes");
        if (list == nullptr || list->type != Json::Type::Array) {
            return Result<std::vector<Morpheme>>::failure("reading worker returned no morphemes array");
        }
        std::vector<Morpheme> morphemes;
        for (const auto& item : list->array) {
            if (item.type != Json::Type::Object) {
                continue;
            }
            Morpheme morpheme;
            morpheme.surface = item.field_string("surface");
            morpheme.reading = item.field_string("reading");
            morpheme.part_of_speech = item.field_string("part_of_speech");
            morpheme.dictionary_form = item.field_string("dictionary_form");
            if (const Json* start = item.field("start");
                start != nullptr && start->type == Json::Type::Number && start->number >= 0) {
                morpheme.start_byte = static_cast<std::size_t>(start->number);
            }
            if (const Json* end = item.field("end");
                end != nullptr && end->type == Json::Type::Number && end->number >= 0) {
                morpheme.end_byte = static_cast<std::size_t>(end->number);
            }
            morphemes.push_back(std::move(morpheme));
        }
        return Result<std::vector<Morpheme>>::success(std::move(morphemes));
    }

private:
    TextWorker worker_;
    std::string last_error_;
};

}  // namespace gekko::yomi::kakasi

GEKKO_YOMI_PLUGIN_EXPORT
void gekko_yomi_register(gekko::yomi::Host* host) {
    host->register_reading(
        {"kakasi", "pykakasi worker. Readings are kana for the surface; the dictionary form is that surface."},
        [](std::string*) -> std::unique_ptr<gekko::yomi::ReadingProvider> {
            return std::make_unique<gekko::yomi::kakasi::KakasiReading>();
        });
}
