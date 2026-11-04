#pragma once

#include "gekko_yomi/frame.hpp"
#include "gekko_yomi/result.hpp"

#include <string>
#include <vector>

namespace gekko::yomi {

// One character (or one grapheme the engine treats as a unit) and where
// it sits in the frame. Coordinates are image pixels.
struct CharBox {
    std::string text;
    Rect box;
    float confidence{0};
};

// A line or a text block. characters is empty when the engine only has
// a string. provides_character_boxes() on the engine says which.
// character offsets are not used here; the boxes are the positions.
struct TextRun {
    std::string text;
    Rect box;
    bool vertical{false};
    float confidence{0};
    std::vector<CharBox> characters;
};

struct OcrRequest {
    // Empty means the engine's default model.
    // A llama-run engine expects the model name the OpenAI API uses.
    std::string model;
};

struct OcrPass {
    std::string engine_id;
    std::string model;
    std::vector<TextRun> runs;
};

// Image to text. Local ONNX models and an OpenAI-compatible vision model
// (llama-run) are both implementations. They differ in
// provides_character_boxes(), not in the method signature.
class OcrEngine {
public:
    virtual ~OcrEngine() = default;

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;
    virtual bool provides_character_boxes() const = 0;

    virtual Result<OcrPass> recognize(const Frame& frame, const OcrRequest& request) = 0;
};

}  // namespace gekko::yomi
