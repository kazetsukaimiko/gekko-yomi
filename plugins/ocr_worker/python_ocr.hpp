#pragma once

#include "gekko_yomi/ocr.hpp"

#include "worker_session.hpp"

#include <string>

namespace gekko::yomi::ocr_worker {

// One Python process that speaks the BGRA worker protocol. Each model plugin
// is a spec: id, whether character boxes exist, and which script to launch.
struct PythonOcrSpec {
    std::string id;
    std::string description;
    std::string model;
    bool character_boxes{false};
    const char* script_env{""};
    const char* script_filename{""};
};

class PythonOcrEngine final : public OcrEngine {
public:
    explicit PythonOcrEngine(PythonOcrSpec spec);

    std::string id() const override;
    std::string description() const override;
    bool provides_character_boxes() const override;
    Result<OcrPass> recognize(const Frame& frame, const OcrRequest& request) override;

private:
    PythonOcrSpec spec_;
    WorkerSession session_;
};

}  // namespace gekko::yomi::ocr_worker
