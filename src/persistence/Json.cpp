#include "persistence/Json.hpp"

#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>

namespace spacetrains::persistence {

namespace {

void escape_to(std::string& out, const std::string& text) {
    out += '"';
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<int>(ch));
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Json parse_document() {
        auto value = parse_value();
        skip_space();
        if (pos_ != text_.size()) {
            fail("trailing characters");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error(std::format("JSON parse error at offset {}: {}", pos_, what));
    }

    void skip_space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t')) {
            ++pos_;
        }
    }

    char peek() {
        skip_space();
        if (pos_ >= text_.size()) {
            fail("unexpected end");
        }
        return text_[pos_];
    }

    void expect(char ch) {
        if (peek() != ch) {
            fail(std::format("expected '{}'", ch));
        }
        ++pos_;
    }

    bool consume_word(std::string_view word) {
        if (text_.substr(pos_, word.size()) == word) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    Json parse_value() {
        const char ch = peek();
        if (ch == '{') {
            ++pos_;
            auto object = Json::object();
            if (peek() == '}') {
                ++pos_;
                return object;
            }
            for (;;) {
                if (peek() != '"') {
                    fail("expected a key");
                }
                auto key = parse_string();
                expect(':');
                object.set(std::move(key), parse_value());
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                expect('}');
                return object;
            }
        }
        if (ch == '[') {
            ++pos_;
            auto array = Json::array();
            if (peek() == ']') {
                ++pos_;
                return array;
            }
            for (;;) {
                array.push(parse_value());
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                expect(']');
                return array;
            }
        }
        if (ch == '"') {
            return Json(parse_string());
        }
        if (consume_word("true")) return Json(true);
        if (consume_word("false")) return Json(false);
        if (consume_word("null")) return Json();
        return Json(parse_number());
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < text_.size() && text_[pos_] != '"') {
            char ch = text_[pos_++];
            if (ch == '\\') {
                if (pos_ >= text_.size()) fail("bad escape");
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) fail("bad \\u escape");
                        unsigned code = 0;
                        std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, code, 16);
                        pos_ += 4;
                        // Save files only escape control characters this way.
                        out += static_cast<char>(code < 0x80 ? code : '?');
                        break;
                    }
                    default: fail("bad escape");
                }
            } else {
                out += ch;
            }
        }
        if (pos_ >= text_.size()) fail("unterminated string");
        ++pos_;
        return out;
    }

    double parse_number() {
        double value = 0.0;
        const auto* begin = text_.data() + pos_;
        const auto [end, error] = std::from_chars(begin, text_.data() + text_.size(), value);
        if (error != std::errc()) {
            fail("bad number");
        }
        pos_ += static_cast<std::size_t>(end - begin);
        return value;
    }

    std::string_view text_;
    std::size_t pos_ {0};
};

}  // namespace

Json& Json::set(std::string key, Json value) {
    if (type_ != Type::Object) {
        throw std::runtime_error("JSON: set() on a non-object");
    }
    fields_.emplace_back(std::move(key), std::move(value));
    return fields_.back().second;
}

const Json* Json::find(std::string_view key) const {
    for (const auto& [name, value] : fields_) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

const Json& Json::get(std::string_view key) const {
    if (const auto* value = find(key)) {
        return *value;
    }
    throw std::runtime_error(std::format("JSON: missing key '{}'", key));
}

Json& Json::push(Json value) {
    if (type_ != Type::Array) {
        throw std::runtime_error("JSON: push() on a non-array");
    }
    items_.push_back(std::move(value));
    return items_.back();
}

double Json::number() const {
    if (type_ == Type::Number) {
        return number_;
    }
    if (type_ == Type::String) {
        if (string_ == "inf") return std::numeric_limits<double>::infinity();
        if (string_ == "-inf") return -std::numeric_limits<double>::infinity();
        if (string_ == "nan") return std::numeric_limits<double>::quiet_NaN();
    }
    throw std::runtime_error("JSON: not a number");
}

bool Json::boolean() const {
    if (type_ != Type::Bool) {
        throw std::runtime_error("JSON: not a boolean");
    }
    return bool_;
}

const std::string& Json::string() const {
    if (type_ != Type::String) {
        throw std::runtime_error("JSON: not a string");
    }
    return string_;
}

void Json::dump_to(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number:
            if (std::isnan(number_)) {
                out += "\"nan\"";
            } else if (std::isinf(number_)) {
                out += number_ > 0.0 ? "\"inf\"" : "\"-inf\"";
            } else {
                // Shortest representation that reads back to the same double.
                out += std::format("{}", number_);
            }
            break;
        case Type::String: escape_to(out, string_); break;
        case Type::Array:
            out += '[';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i > 0) out += ',';
                items_[i].dump_to(out);
            }
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (std::size_t i = 0; i < fields_.size(); ++i) {
                if (i > 0) out += ',';
                escape_to(out, fields_[i].first);
                out += ':';
                fields_[i].second.dump_to(out);
            }
            out += '}';
            break;
    }
}

std::string Json::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

Json Json::parse(std::string_view text) {
    return Parser(text).parse_document();
}

}  // namespace spacetrains::persistence
