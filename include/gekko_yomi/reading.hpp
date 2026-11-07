#pragma once

#include "gekko_yomi/result.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace gekko::yomi {

class Runtime;

// One morpheme of a line.
// start_byte and end_byte are UTF-8 offsets into the surface that was
// passed to analyze. end_byte is exclusive.
struct Morpheme {
    std::string surface;
    std::string reading;
    std::string part_of_speech;
    std::string dictionary_form;
    std::size_t start_byte{0};
    std::size_t end_byte{0};
};

class ReadingProvider {
public:
    virtual ~ReadingProvider() = default;

    virtual Runtime* runtime() { return nullptr; }

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;

    virtual Result<std::vector<Morpheme>> analyze(std::string_view surface) = 0;
};

}  // namespace gekko::yomi
