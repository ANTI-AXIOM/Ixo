// Minimal JSON value, parser and serializer (used by the language server and
// by Ixo's json module).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ixo {

struct Json {
    enum K { Null, Bool, Num, Str, Arr, Obj } k = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : k(Bool), b(v) {}
    Json(int v) : k(Num), n(v) {}
    Json(int64_t v) : k(Num), n((double)v) {}
    Json(double v) : k(Num), n(v) {}
    Json(const char* v) : k(Str), s(v) {}
    Json(std::string v) : k(Str), s(std::move(v)) {}
    static Json array() {
        Json j;
        j.k = Arr;
        return j;
    }
    static Json object() {
        Json j;
        j.k = Obj;
        return j;
    }

    bool isNull() const { return k == Null; }
    const Json* get(const std::string& key) const {
        if (k != Obj) return nullptr;
        for (auto& [kk, v] : obj)
            if (kk == key) return &v;
        return nullptr;
    }
    // Path lookup: j.at("params", "textDocument", "uri")
    template <class... Keys>
    const Json* at(const std::string& key, Keys... rest) const {
        const Json* j = get(key);
        if constexpr (sizeof...(rest) == 0)
            return j;
        else
            return j ? j->at(rest...) : nullptr;
    }
    std::string str(const std::string& def = "") const { return k == Str ? s : def; }
    double num(double def = 0) const { return k == Num ? n : def; }
    Json& set(const std::string& key, Json v) {
        k = Obj;
        for (auto& [kk, vv] : obj)
            if (kk == key) {
                vv = std::move(v);
                return vv;
            }
        obj.emplace_back(key, std::move(v));
        return obj.back().second;
    }
    Json& push(Json v) {
        k = Arr;
        arr.push_back(std::move(v));
        return arr.back();
    }

    static Json parse(const std::string& text, std::string* error);
    std::string dump(int indent = -1) const;
};

// Appends s as a JSON string literal (quotes and escapes).
void jsonQuote(std::string& out, const std::string& s);
// Appends a number the way JSON text writes it (whole numbers without a point,
// others in the shortest form that reads back the same, null if not finite).
void jsonNumber(std::string& out, double d);

}  // namespace ixo
