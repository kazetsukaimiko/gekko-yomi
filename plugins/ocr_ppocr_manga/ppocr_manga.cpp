#include "gekko_yomi/host.hpp"
#include "python_ocr.hpp"

#include <memory>

GEKKO_YOMI_PLUGIN_EXPORT
void gekko_yomi_register(gekko::yomi::Host* host) {
    // Line boxes only. The recognizer is CTC over a crop, not a character detector.
    host->register_ocr(
        {"ppocr-manga", "PP-OCRv6 manga ONNX worker (line boxes, no character boxes)", false},
        [](std::string*) -> std::unique_ptr<gekko::yomi::OcrEngine> {
            return std::make_unique<gekko::yomi::ocr_worker::PythonOcrEngine>(gekko::yomi::ocr_worker::PythonOcrSpec{
                "ppocr-manga",
                "PP-OCRv6 manga ONNX worker (line boxes, no character boxes)",
                "Kellenok/PP-OCRv6_manga",
                false,
                "GEKKO_YOMI_PPOCR_MANGA_WORKER",
                "ppocr_manga.py",
            });
        });
}
