#include "json.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ixo {

namespace {

struct JsonParser {
    const std::string& t;
    size_t i = 0;
    std::string err;

    void ws() {
        while (i < t.size() && std::isspace((unsigned char)t[i])) i++;
    }
    bool fail(const std::string& m) {
        if (err.empty()) err = m + " at offset " + std::to_string(i);
        return false;
    }
    bool literal(const char* w) {
        size_t n = std::char_traits<char>::length(w);
        if (t.compare(i, n, w) == 0) {
            i += n;
            return true;
        }
        return false;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool str(std::string& out) {
        if (t[i] != '"') return fail("expected string");
        i++;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size()) return fail("bad escape");
            char e = t[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 > t.size()) return fail("bad \\u escape");
                    unsigned cp = (unsigned)std::strtoul(t.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        unsigned lo = (unsigned)std::strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        if (i >= t.size()) return fail("unterminated string");
        i++;
        return true;
    }
    bool value(Json& out, int depth) {
        if (depth > 500) return fail("nesting too deep");
        ws();
        if (i >= t.size()) return fail("unexpected end");
        char c = t[i];
        if (c == '{') {
            i++;
            out = Json::object();
            ws();
            if (i < t.size() && t[i] == '}') {
                i++;
                return true;
            }
            while (true) {
                ws();
                std::string key;
                if (i >= t.size() || !str(key)) return fail("expected key");
                ws();
                if (i >= t.size() || t[i] != ':') return fail("expected ':'");
                i++;
                Json v;
                if (!value(v, depth + 1)) return false;
                out.obj.emplace_back(std::move(key), std::move(v));
                ws();
                if (i < t.size() && t[i] == ',') {
                    i++;
                    continue;
                }
                if (i < t.size() && t[i] == '}') {
                    i++;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            i++;
            out = Json::array();
            ws();
            if (i < t.size() && t[i] == ']') {
                i++;
                return true;
            }
            while (true) {
                Json v;
                if (!value(v, depth + 1)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (i < t.size() && t[i] == ',') {
                    i++;
                    continue;
                }
                if (i < t.size() && t[i] == ']') {
                    i++;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out = Json(std::string());
            return str(out.s);
        }
        if (literal("true")) {
            out = Json(true);
            return true;
        }
        if (literal("false")) {
            out = Json(false);
            return true;
        }
        if (literal("null")) {
            out = Json();
            return true;
        }
        if (c == '-' || std::isdigit((unsigned char)c)) {
            const char* start = t.c_str() + i;
            char* end = nullptr;
            double d = std::strtod(start, &end);
            if (end == start) return fail("bad number");
            i += (size_t)(end - start);
            out = Json(d);
            return true;
        }
        return fail(std::string("unexpected character '") + c + "'");
    }
};

}  // namespace

void jsonQuote(std::string& o, const std::string& s) {
    o += '"';
    size_t run = 0;  // characters copied as they are, in one go
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        o.append(s, run, i - run);
        run = i + 1;
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default: {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                o += buf;
            }
        }
    }
    o.append(s, run, s.size() - run);
    o += '"';
}

void jsonNumber(std::string& o, double d) {
    if (!std::isfinite(d)) {
        o += "null";
        return;
    }
    char buf[64];
    std::to_chars_result r;
    if (d == std::floor(d) && std::fabs(d) < 1e15)
        r = std::to_chars(buf, buf + sizeof buf, (long long)d);
    else
        r = std::to_chars(buf, buf + sizeof buf, d);  // shortest that reads back the same
    o.append(buf, r.ptr);
}

namespace {

void dumpStr(std::string& o, const std::string& s) { jsonQuote(o, s); }

void dumpRec(const Json& j, std::string& o, int indent, int level) {
    auto nl = [&](int lv) {
        if (indent < 0) return;
        o += '\n';
        o += std::string((size_t)(indent * lv), ' ');
    };
    switch (j.k) {
        case Json::Null: o += "null"; break;
        case Json::Bool: o += j.b ? "true" : "false"; break;
        case Json::Num: jsonNumber(o, j.n); break;
        case Json::Str: dumpStr(o, j.s); break;
        case Json::Arr:
            o += '[';
            for (size_t i = 0; i < j.arr.size(); i++) {
                if (i) o += ',';
                nl(level + 1);
                dumpRec(j.arr[i], o, indent, level + 1);
            }
            if (!j.arr.empty()) nl(level);
            o += ']';
            break;
        case Json::Obj:
            o += '{';
            for (size_t i = 0; i < j.obj.size(); i++) {
                if (i) o += ',';
                nl(level + 1);
                dumpStr(o, j.obj[i].first);
                o += indent < 0 ? ":" : ": ";
                dumpRec(j.obj[i].second, o, indent, level + 1);
            }
            if (!j.obj.empty()) nl(level);
            o += '}';
            break;
    }
}

}  // namespace

Json Json::parse(const std::string& text, std::string* error) {
    JsonParser p{text, 0, {}};
    Json out;
    if (!p.value(out, 0)) {
        if (error) *error = p.err;
        return Json();
    }
    p.ws();
    if (p.i != text.size()) {
        if (error) *error = "trailing characters at offset " + std::to_string(p.i);
        return Json();
    }
    if (error) error->clear();
    return out;
}

std::string Json::dump(int indent) const {
    std::string o;
    dumpRec(*this, o, indent, 0);
    return o;
}

}  // namespace ixo
