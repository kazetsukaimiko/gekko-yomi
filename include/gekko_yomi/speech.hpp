#pragma once

#include "gekko_yomi/result.hpp"

#include <string>
#include <string_view>

namespace gekko::yomi {

// On-demand speech for a line or a single word. Not part of the web MVP.
class SpeechSynth {
public:
    virtual ~SpeechSynth() = default;

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;

    virtual Status speak(std::string_view text) = 0;
};

}  // namespace gekko::yomi
