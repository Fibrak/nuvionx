#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

static const Value kNull;

Value Value::array() {
    Value v;
    v.t_ = Type::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.t_ = Type::Object;
    return v;
}

const Value& Value::operator[](const char* key) const {
    if (t_ != Type::Object) return kNull;
    for (size_t i = 0; i < keys_.size(); i++)
        if (keys_[i] == key) return arr_[i];
    return kNull;
}

const Value& Value::at(size_t i) const {
    if ((t_ != Type::Array && t_ != Type::Object) || i >= arr_.size()) return kNull;
    return arr_[i];
}

bool Value::has(const char* key) const {
    if (t_ != Type::Object) return false;
    for (auto& k : keys_)
        if (k == key) return true;
    return false;
}

static std::string formatNumber(double n) {
    char buf[64];
    if (std::isfinite(n) && std::fabs(n) < 9.0e15 && n == (double)(long long)n)
        snprintf(buf, sizeof buf, "%lld", (long long)n);
    else if (std::isfinite(n))
        snprintf(buf, sizeof buf, "%.17g", n);
    else
        snprintf(buf, sizeof buf, "0");
    return buf;
}

std::string Value::str(const std::string& def) const {
    switch (t_) {
        case Type::String: return s_;
        case Type::Number: return formatNumber(n_);
        case Type::Bool: return b_ ? "true" : "false";
        default: return def;
    }
}

double Value::num(double def) const {
    switch (t_) {
        case Type::Number: return n_;
        case Type::Bool: return b_ ? 1 : 0;
        case Type::String: {
            if (s_.empty()) return def;
            char* end = nullptr;
            double d = strtod(s_.c_str(), &end);
            if (end == s_.c_str()) return def;
            return d;
        }
        default: return def;
    }
}

long long Value::integer(long long def) const {
    double d = num((double)def);
    return (long long)d;
}

bool Value::boolean(bool def) const {
    switch (t_) {
        case Type::Bool: return b_;
        case Type::Number: return n_ != 0;
        case Type::String: return s_ == "true" || s_ == "1";
        default: return def;
    }
}

std::vector<std::string> Value::strings() const {
    std::vector<std::string> out;
    if (t_ == Type::String) {
        if (!s_.empty()) out.push_back(s_);
    } else if (t_ == Type::Array) {
        for (auto& v : arr_)
            if (v.t_ == Type::String && !v.s_.empty()) out.push_back(v.s_);
    }
    return out;
}

Value& Value::set(const std::string& key, Value v) {
    if (t_ != Type::Object) {
        *this = object();
    }
    for (size_t i = 0; i < keys_.size(); i++) {
        if (keys_[i] == key) {
            arr_[i] = std::move(v);
            return arr_[i];
        }
    }
    keys_.push_back(key);
    arr_.push_back(std::move(v));
    return arr_.back();
}

Value& Value::append(const std::string& key, Value v) {
    if (t_ != Type::Object) {
        *this = object();
    }
    keys_.push_back(key);
    arr_.push_back(std::move(v));
    return arr_.back();
}

Value& Value::push(Value v) {
    if (t_ != Type::Array) {
        *this = array();
    }
    arr_.push_back(std::move(v));
    return arr_.back();
}

static void escapeTo(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

void Value::dumpTo(std::string& out) const {
    switch (t_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Number: out += formatNumber(n_); break;
        case Type::String: escapeTo(s_, out); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < arr_.size(); i++) {
                if (i) out += ',';
                arr_[i].dumpTo(out);
            }
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < arr_.size(); i++) {
                if (i) out += ',';
                escapeTo(keys_[i], out);
                out += ':';
                arr_[i].dumpTo(out);
            }
            out += '}';
            break;
    }
}

std::string Value::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

// ---------------------------------------------------------------------------

class Parser {
public:
    Parser(const char* p, const char* e) : p_(p), e_(e) {}

    bool parseDocument(Value& out) {
        ws();
        if (!parseValue(out, 0)) return false;
        ws();
        return p_ == e_;
    }

private:
    const char* p_;
    const char* e_;

    void ws() {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++;
    }

    bool literal(const char* lit) {
        size_t n = strlen(lit);
        if ((size_t)(e_ - p_) < n || memcmp(p_, lit, n) != 0) return false;
        p_ += n;
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& out) {
        if (e_ - p_ < 4) return false;
        out = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p_++;
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parseString(std::string& out) {
        if (p_ >= e_ || *p_ != '"') return false;
        p_++;
        while (p_ < e_) {
            // copy the run up to the next quote or escape in one go
            const char* run = p_;
            while (p_ < e_ && *p_ != '"' && *p_ != '\\') p_++;
            if (p_ > run) out.append(run, (size_t)(p_ - run));
            if (p_ >= e_) return false;
            char c = *p_++;
            if (c == '"') return true;
            if (p_ >= e_) return false;
            char esc = *p_++;
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // surrogate pair
                        if (e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                            p_ += 2;
                            unsigned lo;
                            if (!hex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else
                                cp = 0xFFFD;
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool parseValue(Value& v, int depth) {
        if (depth > 128 || p_ >= e_) return false;
        char c = *p_;
        if (c == '{') {
            p_++;
            v = Value::object();
            ws();
            if (p_ < e_ && *p_ == '}') {
                p_++;
                return true;
            }
            while (true) {
                ws();
                std::string key;
                if (!parseString(key)) return false;
                ws();
                if (p_ >= e_ || *p_ != ':') return false;
                p_++;
                ws();
                Value child;
                if (!parseValue(child, depth + 1)) return false;
                v.keys_.push_back(std::move(key));
                v.arr_.push_back(std::move(child));
                ws();
                if (p_ >= e_) return false;
                if (*p_ == ',') {
                    p_++;
                    continue;
                }
                if (*p_ == '}') {
                    p_++;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            p_++;
            v = Value::array();
            ws();
            if (p_ < e_ && *p_ == ']') {
                p_++;
                return true;
            }
            while (true) {
                ws();
                Value child;
                if (!parseValue(child, depth + 1)) return false;
                v.arr_.push_back(std::move(child));
                ws();
                if (p_ >= e_) return false;
                if (*p_ == ',') {
                    p_++;
                    continue;
                }
                if (*p_ == ']') {
                    p_++;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v = Value(std::string());
            return parseString(v.s_);
        }
        if (c == 't') {
            if (!literal("true")) return false;
            v = Value(true);
            return true;
        }
        if (c == 'f') {
            if (!literal("false")) return false;
            v = Value(false);
            return true;
        }
        if (c == 'n') {
            if (!literal("null")) return false;
            v = Value();
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            // copy into a bounded buffer so strtod never reads past e_
            char buf[64];
            size_t n = 0;
            while (p_ + n < e_ && n < sizeof(buf) - 1) {
                char d = p_[n];
                if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E')
                    n++;
                else
                    break;
            }
            memcpy(buf, p_, n);
            buf[n] = 0;
            char* end = nullptr;
            double d = strtod(buf, &end);
            if (end == buf) return false;
            p_ += (end - buf);
            v = Value(d);
            return true;
        }
        return false;
    }
};

bool parse(const std::string& text, Value& out) {
    const char* p = text.data();
    const char* e = p + text.size();
    // skip UTF-8 BOM
    if (text.size() >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    Parser parser(p, e);
    Value v;
    if (!parser.parseDocument(v)) return false;
    out = std::move(v);
    return true;
}

}  // namespace json
