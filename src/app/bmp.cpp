#include "bmp.hpp"

namespace gekko::yomi {
namespace {

void put_u16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put_u32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

}  // namespace

std::vector<std::uint8_t> encode_bmp(const Frame& frame) {
    std::vector<std::uint8_t> out;
    if (frame.width <= 0 || frame.height <= 0 || frame.stride_bytes < frame.width * 4) {
        return out;
    }
    const std::size_t need =
        static_cast<std::size_t>(frame.stride_bytes) * static_cast<std::size_t>(frame.height);
    if (frame.pixels.size() < need) {
        return out;
    }

    const int row_stride = (frame.width * 3 + 3) & ~3;
    const std::uint32_t pixel_bytes = static_cast<std::uint32_t>(row_stride) * static_cast<std::uint32_t>(frame.height);
    const std::uint32_t header_bytes = 14 + 40;
    out.reserve(header_bytes + pixel_bytes);

    out.push_back('B');
    out.push_back('M');
    put_u32(&out, header_bytes + pixel_bytes);
    put_u32(&out, 0);
    put_u32(&out, header_bytes);

    put_u32(&out, 40);
    put_u32(&out, static_cast<std::uint32_t>(frame.width));
    put_u32(&out, static_cast<std::uint32_t>(frame.height));
    put_u16(&out, 1);
    put_u16(&out, 24);
    put_u32(&out, 0);
    put_u32(&out, pixel_bytes);
    put_u32(&out, 2835);
    put_u32(&out, 2835);
    put_u32(&out, 0);
    put_u32(&out, 0);

    for (int y = frame.height - 1; y >= 0; --y) {
        const std::uint8_t* src =
            frame.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.stride_bytes);
        for (int x = 0; x < frame.width; ++x) {
            const std::uint8_t* px = src + static_cast<std::size_t>(x) * 4;
            out.push_back(px[0]);
            out.push_back(px[1]);
            out.push_back(px[2]);
        }
        const int pad = row_stride - frame.width * 3;
        for (int p = 0; p < pad; ++p) {
            out.push_back(0);
        }
    }
    return out;
}

}  // namespace gekko::yomi
