#pragma once

#include "gekko_yomi/frame.hpp"
#include "gekko_yomi/result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace gekko::yomi {

class Runtime;

// Lists windows and returns their pixels.
// The first implementation is KWin ScreenShot2. PipeWire / the desktop
// portal is another implementation for compositors that are not KWin.
class CaptureSource {
public:
    virtual ~CaptureSource() = default;

    virtual Runtime* runtime() { return nullptr; }

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;

    virtual Result<std::vector<WindowInfo>> list_windows() = 0;
    virtual Result<Frame> grab_window(std::string_view window_id) = 0;

    // region is in image pixels of that window's client area.
    virtual Result<Frame> grab_region(std::string_view window_id, const Rect& region) = 0;
};

}  // namespace gekko::yomi
