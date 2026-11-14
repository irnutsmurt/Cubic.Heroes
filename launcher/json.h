// A small JSON reader for the pipeline worker's events (docs/M9_WORKER_PROTOCOL.md): objects, arrays, strings
// (with \u escapes as UTF-8), numbers, true, false, null. Values are read-only once parsed.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    bool is_null() const { return type == Null; }
    const Value& operator[](const char* k) const
    {
        static const Value none;
        if (type != Object) return none;
        auto it = o.find(k);
        return it == o.end() ? none : it->second;
    }
    const Value& operator[](size_t i) const
    {
        static const Value none;
        return type == Array && i < a.size() ? a[i] : none;
    }
    size_t size() const { return type == Array ? a.size() : type == Object ? o.size() : 0; }
    std::string str(const std::string& dflt = "") const
    {
        if (type == String) return s;
        if (type == Number) {
            char b2[64];
            if (n == (double)(long long)n) snprintf(b2, sizeof b2, "%lld", (long long)n);
            else snprintf(b2, sizeof b2, "%g", n);
            return b2;
        }
        if (type == Bool) return b ? "true" : "false";
        return dflt;
    }
    double num(double dflt = 0) const { return type == Number ? n : dflt; }
    bool truthy() const { return type == Bool ? b : type == Number ? n != 0 : type == String ? !s.empty() : false; }
};

class Parser {
public:
    explicit Parser(const std::string& t) : p_(t.c_str()), e_(t.c_str() + t.size()) {}
    bool parse(Value& out)
    {
        ws();
        if (!value(out)) return false;
        ws();
        return p_ == e_;
    }

private:
    const char* p_;
    const char* e_;
    void ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) p_++; }
    bool lit(const char* w)
    {
        size_t n = strlen(w);
        if ((size_t)(e_ - p_) < n || strncmp(p_, w, n)) return false;
        p_ += n;
        return true;
    }
    static void utf8(std::string& s, uint32_t c)
    {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | c >> 6); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | c >> 12); s += (char)(0x80 | (c >> 6 & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | c >> 18); s += (char)(0x80 | (c >> 12 & 0x3F)); s += (char)(0x80 | (c >> 6 & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    bool hex4(uint32_t& c)
    {
        if (e_ - p_ < 4) return false;
        c = 0;
        for (int i = 0; i < 4; i++) {
            char h = *p_++;
            c <<= 4;
            if (h >= '0' && h <= '9') c |= h - '0';
            else if (h >= 'a' && h <= 'f') c |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') c |= h - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool string(std::string& s)
    {
        if (p_ >= e_ || *p_ != '"') return false;
        p_++;
        while (p_ < e_ && *p_ != '"') {
            if (*p_ == '\\') {
                if (++p_ >= e_) return false;
                char c = *p_++;
                switch (c) {
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case 'r': s += '\r'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'u': {
                    uint32_t u;
                    if (!hex4(u)) return false;
                    if (u >= 0xD800 && u < 0xDC00 && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        p_ += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(s, u);
                    break;
                }
                default: s += c;
                }
            } else {
                s += *p_++;
            }
        }
        if (p_ >= e_) return false;
        p_++;
        return true;
    }
    bool value(Value& v)
    {
        ws();
        if (p_ >= e_) return false;
        const char c = *p_;
        if (c == '{') {
            v.type = Value::Object;
            p_++;
            ws();
            if (p_ < e_ && *p_ == '}') { p_++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p_ >= e_ || *p_ != ':') return false;
                p_++;
                Value x;
                if (!value(x)) return false;
                v.o[k] = std::move(x);
                ws();
                if (p_ < e_ && *p_ == ',') { p_++; continue; }
                if (p_ < e_ && *p_ == '}') { p_++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Value::Array;
            p_++;
            ws();
            if (p_ < e_ && *p_ == ']') { p_++; return true; }
            for (;;) {
                Value x;
                if (!value(x)) return false;
                v.a.push_back(std::move(x));
                ws();
                if (p_ < e_ && *p_ == ',') { p_++; continue; }
                if (p_ < e_ && *p_ == ']') { p_++; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Value::String; return string(v.s); }
        if (lit("true")) { v.type = Value::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Value::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Value::Null; return true; }
        char* end = nullptr;
        v.n = strtod(p_, &end);
        if (end == p_) return false;
        v.type = Value::Number;
        p_ = end;
        return true;
    }
};

inline bool parse(const std::string& text, Value& out) { return Parser(text).parse(out); }

// a string as a JSON string literal
inline std::string quote(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

}  // namespace json
