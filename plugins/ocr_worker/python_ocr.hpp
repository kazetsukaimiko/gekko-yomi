#pragma once

#include "gekko_yomi/ocr.hpp"
#include "gekko_yomi/runtime.hpp"

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

class PythonOcrEngine final : public OcrEngine, public Runtime {
public:
    explicit PythonOcrEngine(PythonOcrSpec spec);
    ~PythonOcrEngine() override;

    std::string id() const override;
    std::string description() const override;
    bool provides_character_boxes() const override;
    Runtime* runtime() override { return this; }
    Result<OcrPass> recognize(const Frame& frame, const OcrRequest& request) override;

    Status start() override;
    void stop() override;
    RuntimeStatus status() override;

private:
    PythonOcrSpec spec_;
    WorkerSession session_;
    std::string last_error_;
};

}  // namespace gekko::yomi::ocr_worker
