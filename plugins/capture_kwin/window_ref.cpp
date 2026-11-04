#include "window_ref.hpp"

#include <cctype>
#include <cmath>
#include <cstring>

namespace gekko::yomi::kwin {
namespace {

bool is_hex(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool is_braced_uuid(std::string_view text) {
    // {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    if (text.size() != 38 || text.front() != '{' || text.back() != '}') {
        return false;
    }
    const int groups[] = {8, 4, 4, 4, 12};
    std::size_t index = 1;
    for (int group = 0; group < 5; ++group) {
        if (group > 0) {
            if (text[index] != '-') {
                return false;
            }
            ++index;
        }
        for (int digit = 0; digit < groups[group]; ++digit) {
            if (!is_hex(text[index])) {
                return false;
            }
            ++index;
        }
    }
    return index == text.size() - 1;
}

bool little_endian() {
    const std::uint16_t one = 1;
    return *reinterpret_cast<const std::uint8_t*>(&one) == 1;
}

void swap_red_blue(std::uint8_t* pixels, int width, int height, int stride_bytes) {
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_bytes);
        for (int x = 0; x < width; ++x) {
            std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
            const std::uint8_t red = px[0];
            px[0] = px[2];
            px[2] = red;
        }
    }
}

}  // namespace

std::optional<std::string> window_uuid_from_runner_id(std::string_view runner_id) {
    if (is_braced_uuid(runner_id)) {
        return std::string(runner_id);
    }
    const auto brace = runner_id.find('{');
    if (brace == std::string_view::npos || brace < 2 || runner_id[brace - 1] != '_') {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < brace - 1; ++i) {
        if (std::isdigit(static_cast<unsigned char>(runner_id[i])) == 0) {
            return std::nullopt;
        }
    }
    const auto uuid = runner_id.substr(brace);
    if (!is_braced_uuid(uuid)) {
        return std::nullopt;
    }
    return std::string(uuid);
}

bool is_capture_window_type(int net_wm_window_type) {
    return net_wm_window_type == 0 || net_wm_window_type == 5;
}

bool convert_qimage_to_bgra(std::uint32_t qimage_format, std::uint8_t* pixels, int width, int height,
                            int stride_bytes) {
    if (width < 0 || height < 0 || stride_bytes < 0) {
        return false;
    }
    if (width == 0 || height == 0) {
        return true;
    }
    if (pixels == nullptr) {
        return false;
    }
    if (stride_bytes < width * 4) {
        return false;
    }
    // 4 RGB32, 5 ARGB32, 6 ARGB32 premultiplied: B,G,R,A on little-endian.
    if (qimage_format == 4 || qimage_format == 5 || qimage_format == 6) {
        return little_endian();
    }
    // 17 RGBA8888, 18 RGBA8888 premultiplied. Byte order does not follow endianness.
    if (qimage_format == 17 || qimage_format == 18) {
        swap_red_blue(pixels, width, height, stride_bytes);
        return true;
    }
    return false;
}

double image_pixels_per_logical(int image_width, double logical_width, double reported_scale) {
    if (image_width > 0 && logical_width > 0) {
        return static_cast<double>(image_width) / logical_width;
    }
    if (reported_scale > 0) {
        return reported_scale;
    }
    return 1;
}

Result<Frame> crop_frame(const Frame& frame, const Rect& region) {
    if (frame.width <= 0 || frame.height <= 0 || frame.stride_bytes < frame.width * 4) {
        return Result<Frame>::failure("frame has no pixels to crop");
    }
    const std::size_t need =
        static_cast<std::size_t>(frame.stride_bytes) * static_cast<std::size_t>(frame.height);
    if (frame.pixels.size() < need) {
        return Result<Frame>::failure("frame buffer is shorter than its stride");
    }
    if (region.width <= 0 || region.height <= 0) {
        return Result<Frame>::failure("region is outside the image");
    }

    int x0 = static_cast<int>(std::floor(region.x));
    int y0 = static_cast<int>(std::floor(region.y));
    int x1 = static_cast<int>(std::ceil(region.x + region.width));
    int y1 = static_cast<int>(std::ceil(region.y + region.height));
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > frame.width) {
        x1 = frame.width;
    }
    if (y1 > frame.height) {
        y1 = frame.height;
    }
    if (x1 <= x0 || y1 <= y0) {
        return Result<Frame>::failure("region is outside the image");
    }

    Frame out;
    out.width = x1 - x0;
    out.height = y1 - y0;
    out.stride_bytes = out.width * 4;
    out.pixels.resize(static_cast<std::size_t>(out.stride_bytes) * static_cast<std::size_t>(out.height));
    for (int row = 0; row < out.height; ++row) {
        const auto* src = frame.pixels.data() +
                          static_cast<std::size_t>(y0 + row) * static_cast<std::size_t>(frame.stride_bytes) +
                          static_cast<std::size_t>(x0) * 4;
        auto* dst = out.pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(out.stride_bytes);
        std::memcpy(dst, src, static_cast<std::size_t>(out.stride_bytes));
    }
    out.mapping = frame.mapping;
    out.mapping.client_inset_image_x -= static_cast<double>(x0);
    out.mapping.client_inset_image_y -= static_cast<double>(y0);
    return Result<Frame>::success(std::move(out));
}

}  // namespace gekko::yomi::kwin
