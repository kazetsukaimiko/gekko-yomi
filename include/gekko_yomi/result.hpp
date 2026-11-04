#pragma once

#include <optional>
#include <string>
#include <utility>

namespace gekko::yomi {

// A failed result has an error string and no value.
// A success has a value and an empty error.
template <typename T>
struct Result {
    std::optional<T> value;
    std::string error;

    static Result success(T v) {
        Result out;
        out.value = std::move(v);
        return out;
    }

    static Result failure(std::string message) {
        Result out;
        out.error = std::move(message);
        return out;
    }

    bool ok() const { return value.has_value() && error.empty(); }
};

struct Status {
    std::string error;

    static Status success() { return {}; }

    static Status failure(std::string message) {
        Status out;
        out.error = std::move(message);
        return out;
    }

    bool ok() const { return error.empty(); }
};

}  // namespace gekko::yomi
