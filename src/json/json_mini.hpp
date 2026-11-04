#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gekko::yomi {

// A small JSON subset: null, bool, number, string, array, object.
// Enough for the OCR worker and the page's request body. Not a general parser.
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type{Type::Null};
    bool boolean{false};
    double number{0};
    std::string text;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    const Json* field(std::string_view name) const;
    std::string field_string(std::string_view name) const;
};

std::optional<Json> json_parse(std::string_view input, std::string* error);

// Quotes and escapes a string, including the surrounding quotes.
std::string json_quote(std::string_view text);

// Compact number. Whole values stay whole.
std::string json_number(double value);

}  // namespace gekko::yomi
