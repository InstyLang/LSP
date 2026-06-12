#include <lsp/json.hpp>

#include <cctype>
#include <cmath>
#include <sstream>

void JSONValue::escapeString(const std::string& in, std::string& out) {
    out.push_back('"');
    for (char ch : in) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", ch & 0xff);
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void JSONValue::serializeInto(std::string& out) const {
    switch (type_) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Type::Number: {
            if (number_ == std::floor(number_) && std::abs(number_) < 1e15) {
                out += std::to_string(static_cast<long long>(number_));
            } else {
                std::ostringstream stream;
                stream << number_;
                out += stream.str();
            }
            break;
        }
        case Type::String:
            escapeString(string_, out);
            break;
        case Type::Array: {
            out.push_back('[');
            for (size_t i = 0; i < array_.size(); ++i) {
                if (i) out.push_back(',');
                array_[i].serializeInto(out);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, value] : object_) {
                if (!first) out.push_back(',');
                first = false;
                escapeString(key, out);
                out.push_back(':');
                value.serializeInto(out);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string JSONValue::serialize() const {
    std::string out;
    serializeInto(out);
    return out;
}

// ---- parser -------------------------------------------------------------

namespace {

struct ParseState {
    const std::string& text;
    size_t pos = 0;

    explicit ParseState(const std::string& t) : text(t) {}

    void skipWhitespace() {
        while (pos < text.size() &&
               std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
    }

    char peek() const { return pos < text.size() ? text[pos] : '\0'; }
    char get() { return pos < text.size() ? text[pos++] : '\0'; }
    bool eof() const { return pos >= text.size(); }
};

JSONValue parseValue(ParseState& state);

JSONValue parseString(ParseState& state) {
    std::string result;
    state.get(); // opening quote
    while (!state.eof()) {
        char ch = state.get();
        if (ch == '"') {
            break;
        }
        if (ch == '\\') {
            char esc = state.get();
            switch (esc) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'u': {
                    std::string hex;
                    for (int i = 0; i < 4 && !state.eof(); ++i) {
                        hex.push_back(state.get());
                    }
                    unsigned code = static_cast<unsigned>(std::stoul(hex, nullptr, 16));
                    // Minimal UTF-8 encoding for the BMP.
                    if (code < 0x80) {
                        result.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        result.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        result.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: result.push_back(esc); break;
            }
        } else {
            result.push_back(ch);
        }
    }
    return JSONValue(result);
}

JSONValue parseNumber(ParseState& state) {
    size_t start = state.pos;
    while (!state.eof()) {
        char ch = state.peek();
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' ||
            ch == '.' || ch == 'e' || ch == 'E') {
            state.get();
        } else {
            break;
        }
    }
    return JSONValue(std::stod(state.text.substr(start, state.pos - start)));
}

JSONValue parseArray(ParseState& state) {
    JSONValue::Array array;
    state.get(); // [
    state.skipWhitespace();
    if (state.peek() == ']') {
        state.get();
        return JSONValue(std::move(array));
    }
    while (!state.eof()) {
        array.push_back(parseValue(state));
        state.skipWhitespace();
        char ch = state.get();
        if (ch == ']') break;
        // ch == ',' : continue
        state.skipWhitespace();
    }
    return JSONValue(std::move(array));
}

JSONValue parseObject(ParseState& state) {
    JSONValue::Object object;
    state.get(); // {
    state.skipWhitespace();
    if (state.peek() == '}') {
        state.get();
        return JSONValue(std::move(object));
    }
    while (!state.eof()) {
        state.skipWhitespace();
        if (state.peek() != '"') {
            break;
        }
        std::string key = parseString(state).getString();
        state.skipWhitespace();
        state.get(); // :
        state.skipWhitespace();
        object[key] = parseValue(state);
        state.skipWhitespace();
        char ch = state.get();
        if (ch == '}') break;
        // ch == ',' : continue
    }
    return JSONValue(std::move(object));
}

JSONValue parseValue(ParseState& state) {
    state.skipWhitespace();
    char ch = state.peek();
    switch (ch) {
        case '"': return parseString(state);
        case '{': return parseObject(state);
        case '[': return parseArray(state);
        case 't': state.pos += 4; return JSONValue(true);
        case 'f': state.pos += 5; return JSONValue(false);
        case 'n': state.pos += 4; return JSONValue();
        default: return parseNumber(state);
    }
}

} // namespace

JSONValue JSONValue::parse(const std::string& text) {
    ParseState state(text);
    return parseValue(state);
}
