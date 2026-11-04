#pragma once

#include "gekko_yomi/geometry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gekko::yomi {

// Pixel contract for every CaptureSource:
//   8-bit BGRA, row-major, first byte is the top-left pixel.
//   stride_bytes is the distance between rows and may exceed width * 4.
//   pixels.size() is at least stride_bytes * height.
// KWin's screenshot pipe is BGRA and premultiplied. Plugins that grab a
// different layout convert into this one before returning.
struct Frame {
    int width{0};
    int height{0};
    int stride_bytes{0};
    std::vector<std::uint8_t> pixels;
    FrameMapping mapping;
};

struct WindowInfo {
    // Opaque id from the capture backend. For KWin this is the window uuid.
    std::string id;
    std::string title;
    std::string app_id;
    Rect geometry;
    bool minimized{false};
};

}  // namespace gekko::yomi
