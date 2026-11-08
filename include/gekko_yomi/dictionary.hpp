#pragma once

#include "gekko_yomi/result.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace gekko::yomi {

class Runtime;

struct Gloss {
    std::string dictionary_form;
    std::string part_of_speech;
    std::vector<std::string> english;
};

class Dictionary {
public:
    virtual ~Dictionary() = default;

    virtual Runtime* runtime() { return nullptr; }

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;

    virtual Result<std::vector<Gloss>> lookup(std::string_view dictionary_form) = 0;
};

}  // namespace gekko::yomi
