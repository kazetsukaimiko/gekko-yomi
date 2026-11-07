#pragma once

#include "gekko_yomi/frame.hpp"
#include "gekko_yomi/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gekko::yomi {

class Runtime;

enum class FontClass {
    Unknown,
    Gothic,
    Mincho,
    Rounded,
};

struct Color {
    std::uint8_t r{0};
    std::uint8_t g{0};
    std::uint8_t b{0};
    std::uint8_t a{255};
};

// t is 0 at the top of the glyph and 1 at the bottom.
struct GradientStop {
    float t{0};
    Color color;
};

// Sampled from the pixels of one glyph. OCR engines do not produce this.
struct InkStyle {
    Color fill;
    Color outline;
    float outline_width_px{0};
    std::vector<GradientStop> gradient;
    FontClass font_class{FontClass::Unknown};
    // Empty means "pick a face from font_class".
    std::string font_family;
};

class StyleSampler {
public:
    virtual ~StyleSampler() = default;

    virtual Runtime* runtime() { return nullptr; }

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;

    // box is in image pixels of frame.
    virtual Result<InkStyle> sample(const Frame& frame, const Rect& box, std::string_view text) = 0;
};

}  // namespace gekko::yomi
