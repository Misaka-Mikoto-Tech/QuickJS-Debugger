#pragma once
// Minimal JSON library for CDP protocol
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    using ObjectType = std::map<std::string, Value>;
    using ArrayType = std::vector<Value>;

    Value() : type_(Type::Null), bool_val_(false), num_val_(0) {}
    Value(std::nullptr_t) : type_(Type::Null), bool_val_(false), num_val_(0) {}
    Value(bool b) : type_(Type::Bool), bool_val_(b), num_val_(0) {}
    Value(int n) : type_(Type::Number), bool_val_(false), num_val_(static_cast<double>(n)) {}
    Value(unsigned int n) : type_(Type::Number), bool_val_(false), num_val_(static_cast<double>(n)) {}
    Value(int64_t n) : type_(Type::Number), bool_val_(false), num_val_(static_cast<double>(n)) {}
    Value(double n) : type_(Type::Number), bool_val_(false), num_val_(n) {}
    Value(const char* s) : type_(Type::String), bool_val_(false), num_val_(0), str_val_(s) {}
    Value(const std::string& s) : type_(Type::String), bool_val_(false), num_val_(0), str_val_(s) {}
    Value(std::string&& s) : type_(Type::String), bool_val_(false), num_val_(0), str_val_(std::move(s)) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool get_bool() const { return bool_val_; }
    double get_number() const { return num_val_; }
    int get_int() const {
        if (!std::isfinite(num_val_) || num_val_ < (std::numeric_limits<int>::min)() ||
            num_val_ > (std::numeric_limits<int>::max)()) throw std::out_of_range("JSON integer range");
        return static_cast<int>(num_val_);
    }
    int64_t get_int64() const {
        if (!std::isfinite(num_val_) || num_val_ < -9223372036854775808.0 ||
            num_val_ >= 9223372036854775808.0) throw std::out_of_range("JSON int64 range");
        return static_cast<int64_t>(num_val_);
    }
    const std::string& get_string() const { return str_val_; }
    const ArrayType& get_array() const { return arr_val_; }
    ArrayType& get_array() { return arr_val_; }
    const ObjectType& get_object() const { return obj_val_; }
    ObjectType& get_object() { return obj_val_; }

    bool has(const std::string& key) const {
        return type_ == Type::Object && obj_val_.count(key) > 0;
    }

    const Value& operator[](const std::string& key) const {
        static const Value null_value;
        if (type_ != Type::Object) return null_value;
        auto it = obj_val_.find(key);
        return it != obj_val_.end() ? it->second : null_value;
    }

    Value& operator[](const std::string& key) {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            obj_val_.clear();
        }
        return obj_val_[key];
    }

    const Value& operator[](size_t index) const {
        static const Value null_value;
        if (type_ != Type::Array || index >= arr_val_.size()) return null_value;
        return arr_val_[index];
    }

    size_t size() const {
        if (type_ == Type::Array) return arr_val_.size();
        if (type_ == Type::Object) return obj_val_.size();
        return 0;
    }

    static Value object() { Value v; v.type_ = Type::Object; return v; }
    static Value array() { Value v; v.type_ = Type::Array; return v; }

    Value& set(const std::string& key, const Value& val) {
        if (type_ != Type::Object) { type_ = Type::Object; obj_val_.clear(); }
        obj_val_[key] = val;
        return *this;
    }

    Value& push(const Value& val) {
        if (type_ != Type::Array) { type_ = Type::Array; arr_val_.clear(); }
        arr_val_.push_back(val);
        return *this;
    }

    static constexpr size_t max_input_bytes = 1024 * 1024;
    static constexpr size_t max_output_bytes = 4 * 1024 * 1024;
    static constexpr size_t max_depth = 64;
    static constexpr size_t max_nodes = 16384;

    // Used by both JSON and the text-only CDP WebSocket transport.
    static bool is_valid_utf8(const std::string& text) {
        for (size_t i = 0; i < text.size();) {
            const auto first = static_cast<unsigned char>(text[i++]);
            if (first < 0x80) continue;
            unsigned code = 0;
            unsigned remaining = 0;
            unsigned minimum = 0;
            if (first >= 0xc2 && first <= 0xdf) { code = first & 0x1f; remaining = 1; minimum = 0x80; }
            else if (first >= 0xe0 && first <= 0xef) { code = first & 0x0f; remaining = 2; minimum = 0x800; }
            else if (first >= 0xf0 && first <= 0xf4) { code = first & 0x07; remaining = 3; minimum = 0x10000; }
            else return false;
            if (remaining > text.size() - i) return false;
            while (remaining--) {
                const auto next = static_cast<unsigned char>(text[i++]);
                if ((next & 0xc0) != 0x80) return false;
                code = (code << 6) | (next & 0x3f);
            }
            if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        }
        return true;
    }

    std::string serialize() const {
        std::string output;
        size_t nodes = 0;
        serialize_to(output, 0, nodes);
        return output;
    }

    static Value parse(const std::string& str) {
        if (str.size() > max_input_bytes || !is_valid_utf8(str)) fail("JSON byte/UTF-8 budget");
        size_t pos = 0, nodes = 0;
        Value result = parse_value(str, pos, 0, nodes);
        skip_ws(str, pos);
        if (pos != str.size()) fail("Trailing JSON data");
        return result;
    }

private:
    Type type_;
    bool bool_val_;
    double num_val_;
    std::string str_val_;
    ArrayType arr_val_;
    ObjectType obj_val_;

    [[noreturn]] static void fail(const char* message) { throw std::runtime_error(message); }

    static void append(std::string& output, const std::string& text) {
        if (text.size() > max_output_bytes - output.size()) fail("JSON output byte budget");
        output += text;
    }

    static void append_char(std::string& output, char value) {
        if (output.size() == max_output_bytes) fail("JSON output byte budget");
        output += value;
    }

    static void serialize_string(std::string& output, const std::string& text) {
        if (!is_valid_utf8(text)) fail("Invalid JSON output UTF-8");
        append_char(output, '"');
        for (const unsigned char c : text) {
            switch (c) {
            case '"': append(output, "\\\""); break;
            case '\\': append(output, "\\\\"); break;
            case '\b': append(output, "\\b"); break;
            case '\f': append(output, "\\f"); break;
            case '\n': append(output, "\\n"); break;
            case '\r': append(output, "\\r"); break;
            case '\t': append(output, "\\t"); break;
            default:
                if (c < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    append(output, "\\u00");
                    append_char(output, hex[c >> 4]);
                    append_char(output, hex[c & 15]);
                } else append_char(output, static_cast<char>(c));
            }
        }
        append_char(output, '"');
    }

    void serialize_to(std::string& output, size_t depth, size_t& nodes) const {
        if (++nodes > max_nodes || depth > max_depth) fail("JSON output structure budget");
        switch (type_) {
        case Type::Null: append(output, "null"); break;
        case Type::Bool: append(output, bool_val_ ? "true" : "false"); break;
        case Type::Number: {
            if (!std::isfinite(num_val_)) fail("Non-finite JSON number");
            char buffer[64];
            const auto conversion = std::to_chars(buffer, buffer + sizeof(buffer), num_val_,
                std::chars_format::general, std::numeric_limits<double>::max_digits10);
            if (conversion.ec != std::errc{}) fail("JSON number serialization");
            append(output, std::string(buffer, conversion.ptr));
            break;
        }
        case Type::String: serialize_string(output, str_val_); break;
        case Type::Array:
            if (depth >= max_depth) fail("JSON output depth budget");
            append_char(output, '[');
            for (size_t i = 0; i < arr_val_.size(); ++i) {
                if (i) append_char(output, ',');
                arr_val_[i].serialize_to(output, depth + 1, nodes);
            }
            append_char(output, ']');
            break;
        case Type::Object:
            if (depth >= max_depth) fail("JSON output depth budget");
            append_char(output, '{');
            { bool first = true;
              for (const auto& entry : obj_val_) {
                  if (++nodes > max_nodes) fail("JSON output node budget");
                  if (!first) append_char(output, ',');
                  first = false;
                  serialize_string(output, entry.first);
                  append_char(output, ':');
                  entry.second.serialize_to(output, depth + 1, nodes);
              }
            }
            append_char(output, '}');
            break;
        }
    }

    static void skip_ws(const std::string& text, size_t& pos) {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
               text[pos] == '\r' || text[pos] == '\n')) ++pos;
    }

    static unsigned hex_quad(const std::string& text, size_t& pos) {
        if (text.size() - pos < 4) fail("Incomplete Unicode escape");
        unsigned result = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = text[pos++];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else fail("Invalid Unicode hex digit");
            result = (result << 4) | digit;
        }
        return result;
    }

    static void encode_utf8(std::string& result, unsigned code) {
        if (code <= 0x7f) result += static_cast<char>(code);
        else if (code <= 0x7ff) {
            result += static_cast<char>(0xc0 | (code >> 6));
            result += static_cast<char>(0x80 | (code & 0x3f));
        } else if (code <= 0xffff) {
            result += static_cast<char>(0xe0 | (code >> 12));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (code & 0x3f));
        } else {
            result += static_cast<char>(0xf0 | (code >> 18));
            result += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (code & 0x3f));
        }
    }

    static std::string parse_string(const std::string& text, size_t& pos) {
        if (pos == text.size() || text[pos++] != '"') fail("Expected JSON string");
        std::string result;
        while (pos < text.size()) {
            const auto c = static_cast<unsigned char>(text[pos++]);
            if (c == '"') return result;
            if (c < 0x20) fail("Unescaped JSON control character");
            if (c != '\\') { result += static_cast<char>(c); continue; }
            if (pos == text.size()) fail("Incomplete JSON escape");
            switch (text[pos++]) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                unsigned code = hex_quad(text, pos);
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (text.size() - pos < 6 || text[pos] != '\\' || text[pos + 1] != 'u')
                        fail("Missing low Unicode surrogate");
                    pos += 2;
                    const unsigned low = hex_quad(text, pos);
                    if (low < 0xdc00 || low > 0xdfff) fail("Invalid low Unicode surrogate");
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) fail("Lone low Unicode surrogate");
                encode_utf8(result, code);
                break;
            }
            default: fail("Unknown JSON escape");
            }
        }
        fail("Unterminated JSON string");
    }

    static bool digit(char c) { return c >= '0' && c <= '9'; }

    static Value parse_number(const std::string& text, size_t& pos) {
        const size_t start = pos;
        if (pos < text.size() && text[pos] == '-') ++pos;
        if (pos == text.size()) fail("Incomplete JSON number");
        if (text[pos] == '0') ++pos;
        else {
            if (text[pos] < '1' || text[pos] > '9') fail("Invalid JSON number");
            while (pos < text.size() && digit(text[pos])) ++pos;
        }
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            if (pos == text.size() || !digit(text[pos])) fail("Missing fractional digits");
            while (pos < text.size() && digit(text[pos])) ++pos;
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
            if (pos == text.size() || !digit(text[pos])) fail("Missing exponent digits");
            while (pos < text.size() && digit(text[pos])) ++pos;
        }
        double value = 0;
        const auto conversion = std::from_chars(text.data() + start, text.data() + pos, value,
            std::chars_format::general);
        if (conversion.ec != std::errc{} || conversion.ptr != text.data() + pos || !std::isfinite(value))
            fail("JSON number outside finite range");
        return Value(value);
    }

    static Value parse_value(const std::string& text, size_t& pos, size_t depth, size_t& nodes) {
        if (++nodes > max_nodes || depth > max_depth) fail("JSON structure budget");
        skip_ws(text, pos);
        if (pos == text.size()) fail("Expected JSON value");
        const char first = text[pos];
        if (first == '"') return Value(parse_string(text, pos));
        if (first == '{' || first == '[') {
            if (depth >= max_depth) fail("JSON depth budget");
            ++pos;
            Value value = first == '{' ? Value::object() : Value::array();
            const char closing = first == '{' ? '}' : ']';
            skip_ws(text, pos);
            if (pos < text.size() && text[pos] == closing) { ++pos; return value; }
            for (;;) {
                skip_ws(text, pos);
                if (first == '{') {
                    if (++nodes > max_nodes) fail("JSON node budget");
                    const std::string key = parse_string(text, pos);
                    if (value.has(key)) fail("Duplicate JSON object key");
                    skip_ws(text, pos);
                    if (pos == text.size() || text[pos++] != ':') fail("Expected JSON colon");
                    value.set(key, parse_value(text, pos, depth + 1, nodes));
                } else value.push(parse_value(text, pos, depth + 1, nodes));
                skip_ws(text, pos);
                if (pos == text.size()) fail("Unterminated JSON container");
                const char delimiter = text[pos++];
                if (delimiter == closing) return value;
                if (delimiter != ',') fail("Expected JSON comma");
            }
        }
        for (const auto& literal : {std::string("true"), std::string("false"), std::string("null")}) {
            if (text.compare(pos, literal.size(), literal) == 0) {
                pos += literal.size();
                if (literal == "null") return Value();
                return Value(literal == "true");
            }
        }
        return parse_number(text, pos);
    }
};

} // namespace json
