#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spacetrains::persistence {

// A small JSON value for save games: objects keep their key order, numbers are doubles
// written in the shortest form that reads back to the same bits (so a loaded game
// continues exactly). Non-finite numbers are written as the strings "inf", "-inf", "nan".
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(int value) : type_(Type::Number), number_(value) {}
    Json(std::size_t value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Json(const char* value) : type_(Type::String), string_(value) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool is_null() const { return type_ == Type::Null; }

    // Object access. set() appends (keys are expected to be unique); get() throws if absent.
    Json& set(std::string key, Json value);
    [[nodiscard]] const Json& get(std::string_view key) const;
    [[nodiscard]] const Json* find(std::string_view key) const;
    [[nodiscard]] const std::vector<std::pair<std::string, Json>>& fields() const { return fields_; }

    // Array access.
    Json& push(Json value);
    [[nodiscard]] const std::vector<Json>& items() const { return items_; }

    [[nodiscard]] double number() const;
    [[nodiscard]] bool boolean() const;
    [[nodiscard]] const std::string& string() const;

    [[nodiscard]] std::string dump() const;
    [[nodiscard]] static Json parse(std::string_view text);

private:
    void dump_to(std::string& out) const;

    Type type_ {Type::Null};
    bool bool_ {false};
    double number_ {0.0};
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> fields_;
};

}  // namespace spacetrains::persistence
