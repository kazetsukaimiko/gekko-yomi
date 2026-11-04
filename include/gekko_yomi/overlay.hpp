#pragma once

#include "gekko_yomi/result.hpp"
#include "gekko_yomi/style.hpp"

#include <optional>
#include <string>
#include <vector>

namespace gekko::yomi {

// A reading placed where the kanji is, in overlay logical pixels.
// Built by the core from OCR boxes, morphemes, and a FrameMapping.
struct PlacedReading {
    std::string surface;
    std::string reading;
    Rect overlay_rect;
    bool vertical{false};
    InkStyle style;
};

enum class PresentMode {
    WholeWindow,
    UnderCursor,
};

// Picks which placed readings are visible. Whole-window shows all of them.
// Under-cursor shows the run near the pointer. The pointer is in the same
// logical pixels as PlacedReading::overlay_rect.
class Presenter {
public:
    virtual ~Presenter() = default;

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;

    virtual std::vector<PlacedReading> select(const std::vector<PlacedReading>& readings,
                                               PresentMode mode,
                                               const std::optional<Point>& cursor) const = 0;
};

// Paints readings on a click-through surface. The web UI does not use this.
// The later shell does, and only when that component is built.
class Overlay {
public:
    virtual ~Overlay() = default;

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;

    virtual Status show(const std::vector<PlacedReading>& readings) = 0;
    virtual void clear() = 0;
};

}  // namespace gekko::yomi
