#pragma once

#include <vector>

namespace gekko::yomi {

// One plus in the tray mark. x and y are in a unit square, origin at the top left.
struct CrescentPlus {
    double x{0};
    double y{0};
};

// A crescent moon drawn as plus marks. The belly is on the left. The horns
// point up and down on the right.
inline std::vector<CrescentPlus> crescent_plus_marks() {
    return {
        {0.62, 0.10},
        {0.46, 0.18},
        {0.34, 0.30},
        {0.26, 0.42},
        {0.22, 0.54},
        {0.26, 0.66},
        {0.34, 0.78},
        {0.46, 0.88},
        {0.62, 0.96},
        {0.50, 0.34},
        {0.44, 0.50},
        {0.50, 0.66},
    };
}

}  // namespace gekko::yomi
