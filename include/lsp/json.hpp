#pragma once

// Minimal JSON value type for the Insty LSP. Supports parsing, building,
// and serialization sufficient for JSON-RPC / LSP messages.

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

class JSONValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Array = std::vector<JSONValue>;
    using Object = std::map<std::string, JSONValue>;

    JSONValue() : type_(Type::Null) {}
    JSONValue(std::nullptr_t) : type_(Type::Null) {}
    JSONValue(bool value) : type_(Type::Bool), bool_(value) {}
    JSONValue(int value) : type_(Type::Number), number_(value) {}
    JSONValue(long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    JSONValue(long long value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    JSONValue(double value) : type_(Type::Number), number_(value) {}
    JSONValue(const char* value) : type_(Type::String), string_(value) {}
    JSONValue(std::string value) : type_(Type::String), string_(std::move(value)) {}
    JSONValue(Array value) : type_(Type::Array), array_(std::move(value)) {}
    JSONValue(Object value) : type_(Type::Object), object_(std::move(value)) {}
    JSONValue(std::initializer_list<JSONValue> values)
        : type_(Type::Array), array_(values) {}

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool getBool(bool fallback = false) const {
        return type_ == Type::Bool ? bool_ : fallback;
    }
    double getNumber(double fallback = 0.0) const {
        return type_ == Type::Number ? number_ : fallback;
    }
    std::string getString(const std::string& fallback = std::string()) const {
        return type_ == Type::String ? string_ : fallback;
    }

    const Array& asArray() const {
        static const Array empty;
        return type_ == Type::Array ? array_ : empty;
    }
    Array& asArray() {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            array_.clear();
        }
        return array_;
    }
    const Object& asObject() const {
        static const Object empty;
        return type_ == Type::Object ? object_ : empty;
    }

    bool contains(const std::string& key) const {
        return type_ == Type::Object && object_.find(key) != object_.end();
    }

    // Read access; returns Null for missing keys / out-of-range indices.
    const JSONValue& operator[](const std::string& key) const {
        static const JSONValue null;
        if (type_ != Type::Object) return null;
        auto it = object_.find(key);
        return it == object_.end() ? null : it->second;
    }
    // Write access; promotes to Object.
    JSONValue& operator[](const std::string& key) {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            object_.clear();
        }
        return object_[key];
    }
    const JSONValue& operator[](const char* key) const {
        return (*this)[std::string(key)];
    }
    JSONValue& operator[](const char* key) { return (*this)[std::string(key)]; }

    void push_back(JSONValue value) {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            array_.clear();
        }
        array_.push_back(std::move(value));
    }

    size_t size() const {
        if (type_ == Type::Array) return array_.size();
        if (type_ == Type::Object) return object_.size();
        if (type_ == Type::String) return string_.size();
        return 0;
    }

    std::string serialize() const;

    static JSONValue parse(const std::string& text);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;

    void serializeInto(std::string& out) const;
    static void escapeString(const std::string& in, std::string& out);
};
