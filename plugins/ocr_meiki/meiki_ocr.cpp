#include "gekko_yomi/host.hpp"
#include "python_ocr.hpp"

#include <memory>

GEKKO_YOMI_PLUGIN_EXPORT
void gekko_yomi_register(gekko::yomi::Host* host) {
    host->register_ocr(
        {"meiki", "meikiocr worker (BGRA in, text runs out)", true},
        [](std::string*) -> std::unique_ptr<gekko::yomi::OcrEngine> {
            return std::make_unique<gekko::yomi::ocr_worker::PythonOcrEngine>(gekko::yomi::ocr_worker::PythonOcrSpec{
                "meiki",
                "meikiocr worker (BGRA in, text runs out)",
                "meiki",
                true,
                "GEKKO_YOMI_MEIKI_WORKER",
                "meiki_ocr.py",
            });
        });
}
