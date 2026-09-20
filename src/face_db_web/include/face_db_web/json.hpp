#pragma once

#ifndef FACE_DB_WEB_JSON_HPP
#define FACE_DB_WEB_JSON_HPP

/**
 * Small read-only JSON reader for the bulk-import endpoints.
 *
 * Strings are returned as string_view into the ORIGINAL buffer (no copy of the
 * multi-megabyte base64 payloads), except for strings containing escapes, which
 * are materialised in scratch storage owned by the parser.
 *
 * Lifetime: the parser must outlive any Value it produced. Keep both in the
 * same scope (which is how every call site here uses it).
 */

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace face_db_web {
namespace json {

struct Value;

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool        boolean = false;
    double      number  = 0.0;
    std::string_view str;
    // std::vector tolerates an incomplete element type (C++17), which keeps
    // Value self-referential without an extra indirection.
    std::vector<Value> array;
    std::vector<std::pair<std::string_view, Value>> object;

    bool isNull()   const { return type == Type::Null; }
    bool isString() const { return type == Type::String; }
    bool isArray()  const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    std::string asString() const { return std::string(str); }

    const Value* find(std::string_view key) const {
        if (!isObject()) return nullptr;
        for (const auto& kv : object) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    /** Object member, or "" when missing / not a string. */
    std::string stringOr(std::string_view key, const std::string& fallback = {}) const {
        const Value* v = find(key);
        return (v && v->isString()) ? v->asString() : fallback;
    }

    double numberOr(std::string_view key, double fallback = 0.0) const {
        const Value* v = find(key);
        return (v && v->type == Type::Number) ? v->number : fallback;
    }

    bool boolOr(std::string_view key, bool fallback = false) const {
        const Value* v = find(key);
        return (v && v->type == Type::Bool) ? v->boolean : fallback;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool parse(Value& out) {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        if (pos_ != text_.size()) {
            fail("trailing content after JSON value");
            return false;
        }
        return true;
    }

    const std::string& error() const { return error_; }

private:
    void fail(const char* msg) {
        if (error_.empty()) error_ = msg;
    }

    void skipWs() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool literal(const char* lit) {
        const size_t n = std::strlen(lit);
        if (text_.compare(pos_, n, lit) != 0) return false;
        pos_ += n;
        return true;
    }

    bool parseValue(Value& out) {
        if (pos_ >= text_.size()) { fail("unexpected end of input"); return false; }
        switch (text_[pos_]) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': {
                out.type = Value::Type::String;
                return parseString(out.str);
            }
            case 't':
                if (!literal("true")) { fail("bad literal"); return false; }
                out.type = Value::Type::Bool; out.boolean = true; return true;
            case 'f':
                if (!literal("false")) { fail("bad literal"); return false; }
                out.type = Value::Type::Bool; out.boolean = false; return true;
            case 'n':
                if (!literal("null")) { fail("bad literal"); return false; }
                out.type = Value::Type::Null; return true;
            default: return parseNumber(out);
        }
    }

    bool parseObject(Value& out) {
        out.type = Value::Type::Object;
        ++pos_;  // '{'
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }
        while (pos_ < text_.size()) {
            skipWs();
            if (text_[pos_] != '"') { fail("expected object key"); return false; }
            std::string_view key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= text_.size() || text_[pos_] != ':') { fail("expected ':'"); return false; }
            ++pos_;
            skipWs();
            Value v;
            if (!parseValue(v)) return false;
            out.object.emplace_back(key, std::move(v));
            skipWs();
            if (pos_ < text_.size() && text_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }
            fail("expected ',' or '}'");
            return false;
        }
        fail("unterminated object");
        return false;
    }

    bool parseArray(Value& out) {
        out.type = Value::Type::Array;
        ++pos_;  // '['
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
        while (pos_ < text_.size()) {
            skipWs();
            Value v;
            if (!parseValue(v)) return false;
            out.array.push_back(std::move(v));
            skipWs();
            if (pos_ < text_.size() && text_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
            fail("expected ',' or ']'");
            return false;
        }
        fail("unterminated array");
        return false;
    }

    bool parseNumber(Value& out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool any = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '-' || c == '+') {
                ++pos_;
                any = true;
            } else {
                break;
            }
        }
        if (!any) { fail("expected value"); return false; }
        const std::string tmp(text_.substr(start, pos_ - start));
        out.type = Value::Type::Number;
        out.number = std::strtod(tmp.c_str(), nullptr);
        return true;
    }

    static void appendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    static unsigned hex4(std::string_view s) {
        unsigned v = 0;
        for (char c : s) {
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
        }
        return v;
    }

    bool parseString(std::string_view& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') { fail("expected string"); return false; }
        ++pos_;
        const size_t start = pos_;
        // Fast path: scan to the closing quote; an escape forces the slow path.
        // This matters for the multi-megabyte base64 payloads of bulk import.
        while (pos_ < text_.size() && text_[pos_] != '"' && text_[pos_] != '\\') ++pos_;

        if (pos_ < text_.size() && text_[pos_] == '"') {
            out = text_.substr(start, pos_ - start);
            ++pos_;
            return true;
        }
        if (pos_ >= text_.size() || text_[pos_] != '\\') {
            fail("unterminated string");
            return false;
        }
        pos_ = start;

        std::string decoded;
        decoded.reserve(pos_ - start + 16);
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '"') { ++pos_; break; }
            if (c != '\\') { decoded.push_back(c); ++pos_; continue; }
            ++pos_;
            if (pos_ >= text_.size()) { fail("bad escape"); return false; }
            const char e = text_[pos_++];
            switch (e) {
                case '"':  decoded.push_back('"');  break;
                case '\\': decoded.push_back('\\'); break;
                case '/':  decoded.push_back('/');  break;
                case 'b':  decoded.push_back('\b'); break;
                case 'f':  decoded.push_back('\f'); break;
                case 'n':  decoded.push_back('\n'); break;
                case 'r':  decoded.push_back('\r'); break;
                case 't':  decoded.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) { fail("bad \\u escape"); return false; }
                    unsigned cp = hex4(text_.substr(pos_, 4));
                    pos_ += 4;
                    // Surrogate pair.
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        const unsigned lo = hex4(text_.substr(pos_ + 2, 4));
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            pos_ += 6;
                        }
                    }
                    appendUtf8(decoded, cp);
                    break;
                }
                default: fail("unknown escape"); return false;
            }
        }
        scratch_.push_back(std::move(decoded));
        out = scratch_.back();
        return true;
    }

    std::string_view text_;
    size_t pos_ = 0;
    std::string error_;
    std::vector<std::string> scratch_;  // owns de-escaped strings
};

/** Convenience one-shot parse. */
inline bool parse(std::string_view text, Value& out, std::string& error) {
    Parser p(text);
    if (!p.parse(out)) {
        error = p.error();
        return false;
    }
    return true;
}

}  // namespace json
}  // namespace face_db_web

#endif  // FACE_DB_WEB_JSON_HPP
