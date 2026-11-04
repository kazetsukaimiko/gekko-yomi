#pragma once

#include "gekko_yomi/frame.hpp"

#include <cstdint>
#include <vector>

namespace gekko::yomi {

// 24-bit bottom-up BMP. Browsers display this without an image library.
// Alpha is dropped. Rows are padded to a multiple of 4 bytes.
std::vector<std::uint8_t> encode_bmp(const Frame& frame);

}  // namespace gekko::yomi
