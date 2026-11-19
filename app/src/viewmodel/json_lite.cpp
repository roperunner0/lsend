/*
 * Minimal JSON reader/writer for the lsend control protocol.
 * SPDX-License-Identifier: MIT
 */

#include "json_lite.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

namespace json {
namespace {

const Value kNull{};

struct Parser {
    const char* p;
    const char* end;
    int depth{0};

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            ++p;
        }
    }

    bool lit(const char* word) {
        const size_t len = std::strlen(word);
        if (static_cast<size_t>(end - p) < len || std::strncmp(p, word, len) != 0) {
            return false;
        }
        p += len;
        return true;
    }

    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& cp) {
        if (end - p < 4) {
            return false;
        }
        char buf[5] = {p[0], p[1], p[2], p[3], 0};
        char* stop = nullptr;
        cp = static_cast<unsigned>(std::strtoul(buf, &stop, 16));
        if (stop != buf + 4) {
            return false;
        }
        p += 4;
        return true;
    }

    bool string(std::string& out) {
        if (p >= end || *p != '"') {
            return false;
        }
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                if (p >= end) {
                    return false;
                }
                const char c = *p++;
                switch (c) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!hex4(cp)) {
                            return false;
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' &&
                            p[1] == 'u') {
                            p += 2;
                            unsigned lo = 0;
                            if (!hex4(lo)) {
                                return false;
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        put_utf8(out, cp);
                        break;
                    }
                    default: out += c; break;  // \" \\ \/
                }
            } else {
                out += *p++;
            }
        }
        if (p >= end) {
            return false;
        }
        ++p;
        return true;
    }

    bool value(Value& v) {
        if (++depth > 32) {
            return false;
        }
        ws();
        if (p >= end) {
            return false;
        }
        bool ok = true;
        if (*p == '{') {
            v.type = Value::Type::Object;
            ++p;
            ws();
            if (p < end && *p == '}') {
                ++p;
            } else {
                while (ok) {
                    ws();
                    std::string key;
                    if (!string(key)) {
                        return false;
                    }
                    ws();
                    if (p >= end || *p != ':') {
                        return false;
                    }
                    ++p;
                    Value child;
                    if (!value(child)) {
                        return false;
                    }
                    v.obj.emplace_back(std::move(key), std::move(child));
                    ws();
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (p < end && *p == '}') {
                        ++p;
                        break;
                    }
                    ok = false;
                }
            }
        } else if (*p == '[') {
            v.type = Value::Type::Array;
            ++p;
            ws();
            if (p < end && *p == ']') {
                ++p;
            } else {
                while (ok) {
                    Value child;
                    if (!value(child)) {
                        return false;
                    }
                    v.arr.push_back(std::move(child));
                    ws();
                    if (p < end && *p == ',') {
                        ++p;
                        continue;
                    }
                    if (p < end && *p == ']') {
                        ++p;
                        break;
                    }
                    ok = false;
                }
            }
        } else if (*p == '"') {
            v.type = Value::Type::String;
            ok = string(v.s);
        } else if (lit("true")) {
            v.type = Value::Type::Bool;
            v.b = true;
        } else if (lit("false")) {
            v.type = Value::Type::Bool;
        } else if (lit("null")) {
            v.type = Value::Type::Null;
        } else {
            char* stop = nullptr;
            std::string num(p, std::min<size_t>(static_cast<size_t>(end - p), 64));
            v.n = std::strtod(num.c_str(), &stop);
            if (stop == num.c_str()) {
                return false;
            }
            v.type = Value::Type::Number;
            p += stop - num.c_str();
        }
        --depth;
        return ok;
    }
};

} // namespace

const Value& Value::operator[](const char* key) const {
    if (type == Type::Object) {
        for (const auto& kv : obj) {
            if (kv.first == key) {
                return kv.second;
            }
        }
    }
    return kNull;
}

const Value& Value::operator[](size_t i) const {
    if (type == Type::Array && i < arr.size()) {
        return arr[i];
    }
    return kNull;
}

bool parse(const std::string& text, Value& out) {
    out = Value{};
    Parser parser{text.data(), text.data() + text.size()};
    if (!parser.value(out)) {
        return false;
    }
    parser.ws();
    return parser.p == parser.end;
}

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

} // namespace json
