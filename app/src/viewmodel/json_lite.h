/*
 * Minimal JSON reader/writer for the lsend control protocol (one object per line).
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type{Type::Null};
    bool b{false};
    double n{0};
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    const Value& operator[](const char* key) const;  // Null value if missing
    const Value& operator[](size_t i) const;
    size_t size() const { return type == Type::Array ? arr.size() : obj.size(); }

    bool is_null() const { return type == Type::Null; }
    std::string str(const std::string& def = "") const { return type == Type::String ? s : def; }
    long long integer(long long def = 0) const {
        return type == Type::Number ? static_cast<long long>(n) : def;
    }
    bool boolean(bool def = false) const { return type == Type::Bool ? b : def; }
};

// Returns false on malformed input.
bool parse(const std::string& text, Value& out);

// Quote and escape a string for JSON output.
std::string quote(const std::string& text);

} // namespace json
