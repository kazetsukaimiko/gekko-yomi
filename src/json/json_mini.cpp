#include "json_mini.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gekko::yomi {
namespace {

struct Parser {
    std::string_view in;
    std::size_t i{0};
    std::string* error{nullptr};
    int depth{0};

    bool fail(const std::string& message) {
        if (error != nullptr && error->empty()) {
            *error = message;
        }
        return false;
    }

    void skip() {
        while (i < in.size() && std::isspace(static_cast<unsigned char>(in[i])) != 0) {
            ++i;
        }
    }

    bool parse_value(Json* out) {
        skip();
        if (i >= in.size()) {
            return fail("unexpected end of json");
        }
        if (depth > 12) {
            return fail("json is nested too deep");
        }
        const char c = in[i];
        if (c == '{') {
            return parse_object(out);
        }
        if (c == '[') {
            return parse_array(out);
        }
        if (c == '"') {
            out->type = Json::Type::String;
            return parse_string(&out->text);
        }
        if (c == 't' || c == 'f') {
            return parse_bool(out);
        }
        if (c == 'n') {
            return parse_null(out);
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
            return parse_number(out);
        }
        return fail(std::string("unexpected json character: ") + c);
    }

    bool parse_object(Json* out) {
        ++i;
        ++depth;
        out->type = Json::Type::Object;
        skip();
        if (i < in.size() && in[i] == '}') {
            ++i;
            --depth;
            return true;
        }
        while (i < in.size()) {
            skip();
            std::string key;
            if (!parse_string(&key)) {
                return false;
            }
            skip();
            if (i >= in.size() || in[i] != ':') {
                return fail("expected ':' in json object");
            }
            ++i;
            Json value;
            if (!parse_value(&value)) {
                return false;
            }
            out->object.emplace_back(std::move(key), std::move(value));
            skip();
            if (i < in.size() && in[i] == ',') {
                ++i;
                continue;
            }
            if (i < in.size() && in[i] == '}') {
                ++i;
                --depth;
                return true;
            }
            return fail("expected ',' or '}' in json object");
        }
        return fail("unclosed json object");
    }

    bool parse_array(Json* out) {
        ++i;
        ++depth;
        out->type = Json::Type::Array;
        skip();
        if (i < in.size() && in[i] == ']') {
            ++i;
            --depth;
            return true;
        }
        while (i < in.size()) {
            Json value;
            if (!parse_value(&value)) {
                return false;
            }
            out->array.push_back(std::move(value));
            skip();
            if (i < in.size() && in[i] == ',') {
                ++i;
                continue;
            }
            if (i < in.size() && in[i] == ']') {
                ++i;
                --depth;
                return true;
            }
            return fail("expected ',' or ']' in json array");
        }
        return fail("unclosed json array");
    }

    bool parse_string(std::string* out) {
        if (i >= in.size() || in[i] != '"') {
            return fail("expected a json string");
        }
        ++i;
        out->clear();
        while (i < in.size()) {
            const char c = in[i++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (i >= in.size()) {
                    return fail("truncated json escape");
                }
                const char e = in[i++];
                switch (e) {
                    case '"':
                    case '\\':
                    case '/':
                        out->push_back(e);
                        break;
                    case 'b':
                        out->push_back('\b');
                        break;
                    case 'f':
                        out->push_back('\f');
                        break;
                    case 'n':
                        out->push_back('\n');
                        break;
                    case 'r':
                        out->push_back('\r');
                        break;
                    case 't':
                        out->push_back('\t');
                        break;
                    case 'u':
                        if (!parse_u_escape(out)) {
                            return false;
                        }
                        break;
                    default:
                        return fail("unknown json escape");
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("raw control character in json string");
            }
            out->push_back(c);
        }
        return fail("unclosed json string");
    }

    bool parse_u_escape(std::string* out) {
        if (!append_codepoint(out, read_hex4())) {
            return false;
        }
        return true;
    }

    int read_hex4() {
        if (i + 4 > in.size()) {
            fail("short unicode escape");
            return -1;
        }
        int value = 0;
        for (int n = 0; n < 4; ++n) {
            const char c = in[i++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value += c - '0';
            } else if (c >= 'a' && c <= 'f') {
                value += c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                value += c - 'A' + 10;
            } else {
                fail("bad unicode escape");
                return -1;
            }
        }
        return value;
    }

    bool append_codepoint(std::string* out, int code) {
        if (code < 0) {
            return false;
        }
        // A leading surrogate must be paired. Lone surrogates become U+FFFD.
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (i + 2 <= in.size() && in[i] == '\\' && in[i + 1] == 'u') {
                i += 2;
                const int low = read_hex4();
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else {
                    code = 0xFFFD;
                }
            } else {
                code = 0xFFFD;
            }
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            code = 0xFFFD;
        }
        append_utf8(out, code);
        return true;
    }

    static void append_utf8(std::string* out, int code) {
        const auto u = static_cast<unsigned>(code);
        if (u < 0x80) {
            out->push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (u >> 6)));
            out->push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else if (u < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (u >> 12)));
            out->push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (u >> 18)));
            out->push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }

    bool parse_bool(Json* out) {
        out->type = Json::Type::Bool;
        if (in.substr(i, 4) == "true") {
            out->boolean = true;
            i += 4;
            return true;
        }
        if (in.substr(i, 5) == "false") {
            out->boolean = false;
            i += 5;
            return true;
        }
        return fail("expected true or false");
    }

    bool parse_null(Json* out) {
        if (in.substr(i, 4) != "null") {
            return fail("expected null");
        }
        out->type = Json::Type::Null;
        i += 4;
        return true;
    }

    bool parse_number(Json* out) {
        const std::size_t start = i;
        if (in[i] == '-') {
            ++i;
        }
        if (i >= in.size() || std::isdigit(static_cast<unsigned char>(in[i])) == 0) {
            return fail("expected a digit");
        }
        while (i < in.size() && std::isdigit(static_cast<unsigned char>(in[i])) != 0) {
            ++i;
        }
        if (i < in.size() && in[i] == '.') {
            ++i;
            if (i >= in.size() || std::isdigit(static_cast<unsigned char>(in[i])) == 0) {
                return fail("expected a digit after decimal point");
            }
            while (i < in.size() && std::isdigit(static_cast<unsigned char>(in[i])) != 0) {
                ++i;
            }
        }
        if (i < in.size() && (in[i] == 'e' || in[i] == 'E')) {
            ++i;
            if (i < in.size() && (in[i] == '+' || in[i] == '-')) {
                ++i;
            }
            if (i >= in.size() || std::isdigit(static_cast<unsigned char>(in[i])) == 0) {
                return fail("expected an exponent");
            }
            while (i < in.size() && std::isdigit(static_cast<unsigned char>(in[i])) != 0) {
                ++i;
            }
        }
        const std::string token(in.substr(start, i - start));
        char* end = nullptr;
        out->number = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size()) {
            return fail("bad json number");
        }
        out->type = Json::Type::Number;
        return true;
    }
};

}  // namespace

const Json* Json::field(std::string_view name) const {
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto& entry : object) {
        if (entry.first == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

std::string Json::field_string(std::string_view name) const {
    const Json* value = field(name);
    if (value == nullptr || value->type != Type::String) {
        return {};
    }
    return value->text;
}

std::optional<Json> json_parse(std::string_view input, std::string* error) {
    Parser parser;
    parser.in = input;
    parser.error = error;
    Json value;
    if (!parser.parse_value(&value)) {
        return std::nullopt;
    }
    parser.skip();
    if (parser.i != input.size()) {
        parser.fail("trailing data after json");
        return std::nullopt;
    }
    return value;
}

std::string json_quote(std::string_view text) {
    std::string out;
    out.push_back('"');
    for (unsigned char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string json_number(double value) {
    if (!std::isfinite(value)) {
        return "0";
    }
    const double nearest = std::round(value);
    if (std::abs(value - nearest) < 1e-6 && std::abs(nearest) < 1e15) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f", nearest);
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", value);
    return buf;
}

}  // namespace gekko::yomi
