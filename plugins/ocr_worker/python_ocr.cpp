#include "python_ocr.hpp"

#include "json_mini.hpp"

#include <utility>

namespace gekko::yomi::ocr_worker {
namespace {

Rect box_from_json(const Json& value) {
    Rect box;
    if (value.type != Json::Type::Array || value.array.size() < 4) {
        return box;
    }
    if (value.array[0].type == Json::Type::Number) {
        box.x = value.array[0].number;
    }
    if (value.array[1].type == Json::Type::Number) {
        box.y = value.array[1].number;
    }
    if (value.array[2].type == Json::Type::Number) {
        box.width = value.array[2].number;
    }
    if (value.array[3].type == Json::Type::Number) {
        box.height = value.array[3].number;
    }
    return box;
}

}  // namespace

PythonOcrEngine::PythonOcrEngine(PythonOcrSpec spec) : spec_(std::move(spec)) {}

std::string PythonOcrEngine::id() const { return spec_.id; }

std::string PythonOcrEngine::description() const { return spec_.description; }

bool PythonOcrEngine::provides_character_boxes() const { return spec_.character_boxes; }

Result<OcrPass> PythonOcrEngine::recognize(const Frame& frame, const OcrRequest&) {
    std::string error;
    if (!session_.running() &&
        !session_.start(default_python(), worker_script(spec_.script_env, spec_.script_filename), &error)) {
        return Result<OcrPass>::failure(error);
    }
    if (frame.width <= 0 || frame.height <= 0 || frame.stride_bytes < frame.width * 4) {
        return Result<OcrPass>::failure("frame has no pixels");
    }
    const std::size_t nbytes =
        static_cast<std::size_t>(frame.stride_bytes) * static_cast<std::size_t>(frame.height);
    if (frame.pixels.size() < nbytes) {
        return Result<OcrPass>::failure("frame buffer is shorter than its stride");
    }
    const std::string header = std::string("{\"width\":") + std::to_string(frame.width) +
                               ",\"height\":" + std::to_string(frame.height) +
                               ",\"stride\":" + std::to_string(frame.stride_bytes) + "}";
    // First-run weight download can take a while. Later calls are the model.
    const auto line = session_.request(header, frame.pixels.data(), nbytes, 180000, &error);
    if (!line) {
        session_.close();
        return Result<OcrPass>::failure(error);
    }
    const auto json = json_parse(*line, &error);
    if (!json) {
        return Result<OcrPass>::failure("OCR worker returned invalid json: " + error);
    }
    if (const Json* worker_error = json->field("error");
        worker_error != nullptr && worker_error->type == Json::Type::String && !worker_error->text.empty()) {
        return Result<OcrPass>::failure(worker_error->text);
    }
    const Json* runs = json->field("runs");
    if (runs == nullptr || runs->type != Json::Type::Array) {
        return Result<OcrPass>::failure("OCR worker returned no runs array");
    }
    OcrPass pass;
    pass.engine_id = id();
    pass.model = spec_.model;
    for (const auto& item : runs->array) {
        if (item.type != Json::Type::Object) {
            continue;
        }
        TextRun run;
        run.text = item.field_string("text");
        if (const Json* vertical = item.field("vertical");
            vertical != nullptr && vertical->type == Json::Type::Bool) {
            run.vertical = vertical->boolean;
        }
        if (const Json* confidence = item.field("confidence");
            confidence != nullptr && confidence->type == Json::Type::Number) {
            run.confidence = static_cast<float>(confidence->number);
        }
        if (const Json* box = item.field("box")) {
            run.box = box_from_json(*box);
        }
        if (const Json* characters = item.field("characters");
            characters != nullptr && characters->type == Json::Type::Array) {
            for (const auto& character : characters->array) {
                CharBox box;
                box.text = character.field_string("text");
                if (const Json* raw = character.field("box")) {
                    box.box = box_from_json(*raw);
                }
                if (const Json* confidence = character.field("confidence");
                    confidence != nullptr && confidence->type == Json::Type::Number) {
                    box.confidence = static_cast<float>(confidence->number);
                }
                run.characters.push_back(std::move(box));
            }
        }
        pass.runs.push_back(std::move(run));
    }
    return Result<OcrPass>::success(std::move(pass));
}

}  // namespace gekko::yomi::ocr_worker
