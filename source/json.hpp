// Minimal JSON DOM + parser/serializer (no exceptions, no external deps).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : t_(Type::Bool), b_(v) {}
    Value(int v) : t_(Type::Number), n_(v) {}
    Value(long v) : t_(Type::Number), n_((double)v) {}
    Value(long long v) : t_(Type::Number), n_((double)v) {}
    Value(double v) : t_(Type::Number), n_(v) {}
    Value(const char* v) : t_(Type::String), s_(v ? v : "") {}
    Value(const std::string& v) : t_(Type::String), s_(v) {}

    static Value array();
    static Value object();

    Type type() const { return t_; }
    bool isNull() const { return t_ == Type::Null; }
    bool isBool() const { return t_ == Type::Bool; }
    bool isNumber() const { return t_ == Type::Number; }
    bool isString() const { return t_ == Type::String; }
    bool isArray() const { return t_ == Type::Array; }
    bool isObject() const { return t_ == Type::Object; }

    size_t size() const { return (t_ == Type::Array || t_ == Type::Object) ? arr_.size() : 0; }

    // Lookups never fail: missing keys / indices return a shared null value.
    const Value& operator[](const char* key) const;
    const Value& operator[](const std::string& key) const { return (*this)[key.c_str()]; }
    const Value& at(size_t i) const;
    bool has(const char* key) const;

    // Array elements, or object values (parallel to keys()).
    const std::vector<Value>& items() const { return arr_; }
    const std::vector<std::string>& keys() const { return keys_; }

    std::string str(const std::string& def = "") const;
    double num(double def = 0) const;
    long long integer(long long def = 0) const;
    bool boolean(bool def = false) const;
    // Array of strings (non-strings skipped); a single string becomes a 1-element list.
    std::vector<std::string> strings() const;

    Value& set(const std::string& key, Value v);  // turns null into object
    Value& push(Value v);                          // turns null into array

    std::string dump() const;

private:
    friend class Parser;
    Type t_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> arr_;
    std::vector<std::string> keys_;
    void dumpTo(std::string& out) const;
};

bool parse(const std::string& text, Value& out);

}  // namespace json
