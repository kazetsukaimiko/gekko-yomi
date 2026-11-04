#include "gekko_yomi/geometry.hpp"

namespace gekko::yomi {

Rect image_rect_to_overlay(const Rect& image, const FrameMapping& mapping) {
    const double scale =
        mapping.image_pixels_per_logical == 0.0 ? 1.0 : mapping.image_pixels_per_logical;
    Rect out;
    out.x = mapping.window_logical_x + (image.x - mapping.client_inset_image_x) / scale;
    out.y = mapping.window_logical_y + (image.y - mapping.client_inset_image_y) / scale;
    out.width = image.width / scale;
    out.height = image.height / scale;
    return out;
}

}  // namespace gekko::yomi
