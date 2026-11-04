#pragma once

namespace gekko::yomi {

// Axis-aligned rectangle. x and y are the top-left corner.
// The coordinate space is named by the value that carries the rect
// (a frame, a window, or an overlay), not by the rect itself.
struct Rect {
    double x{0};
    double y{0};
    double width{0};
    double height{0};
};

struct Point {
    double x{0};
    double y{0};
};

// How a captured image sits on the screen.
// image_rect_to_overlay is the only conversion. Callers do not scale.
struct FrameMapping {
    // Top-left of the window in logical screen pixels.
    double window_logical_x{0};
    double window_logical_y{0};

    // Captured pixels per logical pixel. 1.0 when one image pixel is one
    // logical pixel. A value of 2 means the image is twice as dense.
    double image_pixels_per_logical{1};

    // Image-pixel inset of the client origin inside the capture.
    // Zero when the capture is the client area and starts at its top-left.
    double client_inset_image_x{0};
    double client_inset_image_y{0};
};

Rect image_rect_to_overlay(const Rect& image, const FrameMapping& mapping);

}  // namespace gekko::yomi
