#pragma once

#include "gekko_yomi/frame.hpp"
#include "gekko_yomi/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gekko::yomi::kwin {

// WindowsRunner ids look like "0_{uuid}". CaptureWindow wants the braced uuid.
// Desktop rows from the same Match call have no braces and are rejected.
std::optional<std::string> window_uuid_from_runner_id(std::string_view runner_id);

// NET::Normal and NET::Dialog. Other types stay out of the picker.
bool is_capture_window_type(int net_wm_window_type);

// KWin 6.8 on this machine writes QImage::Format_RGBA8888_Premultiplied (18).
// Formats 17 and 18 are R,G,B,A in memory. Formats 4, 5, and 6 are B,G,R,A
// on a little-endian machine. The frame contract is BGRA, so 17 and 18 are
// swapped here. Anything else fails and the format number is reported.
bool convert_qimage_to_bgra(std::uint32_t qimage_format, std::uint8_t* pixels, int width, int height,
                            int stride_bytes);

// Prefer image width / logical width. The screenshot "scale" field is only a
// fallback for when the window geometry is missing. A logical-sized image on
// a 1.5x screen has a ratio near 1 even though that field says 1.5.
double image_pixels_per_logical(int image_width, double logical_width, double reported_scale);

// region is in image pixels. The cropped frame's mapping inset shifts so
// image_rect_to_overlay still lands on the same screen point.
Result<Frame> crop_frame(const Frame& frame, const Rect& region);

}  // namespace gekko::yomi::kwin
