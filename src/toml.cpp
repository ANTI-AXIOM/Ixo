#include "toml.hpp"

#include <cctype>
#include <filesystem>

#include "bundle.hpp"

namespace ixo {

namespace {

struct TomlParser {
    const std::string& t;
    size_t i = 0;
    int line = 1;
    std::string err;

    explicit TomlParser(const std::string& text) : t(text) {}

    bool fail(const std::string& m) {
        if (err.empty()) err = "line " + std::to_string(line) + ": " + m;
        return false;
    }
    void skipSpace() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
    }
    void skipComment() {
        skipSpace();
        if (i < t.size() && t[i] == '#')
            while (i < t.size() && t[i] != '\n') i++;
    }
    // whitespace, newlines and comments (inside arrays)
    void skipAll() {
        while (i < t.size()) {
            if (t[i] == '\n') {
                line++;
                i++;
            } else if (t[i] == ' ' || t[i] == '\t' || t[i] == '\r') {
                i++;
            } else if (t[i] == '#') {
                while (i < t.size() && t[i] != '\n') i++;
            } else {
                break;
            }
        }
    }
    bool key(std::string& out) {
        skipSpace();
        if (i < t.size() && (t[i] == '"' || t[i] == '\'')) {
            TomlValue v;
            if (!value(v)) return false;
            out = v.s;
            return true;
        }
        size_t s = i;
        while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '_' || t[i] == '-' || t[i] == '.')) i++;
        if (s == i) return fail("expected a key");
        out = t.substr(s, i - s);
        return true;
    }
    bool value(TomlValue& v) {
        skipSpace();
        if (i >= t.size()) return fail("expected a value");
        char c = t[i];
        if (c == '"' || c == '\'') {
            bool literal = c == '\'';
            i++;
            v.k = TomlValue::Str;
            while (i < t.size() && t[i] != c) {
                if (t[i] == '\n') return fail("unterminated string");
                if (!literal && t[i] == '\\' && i + 1 < t.size()) {
                    char e = t[++i];
                    v.s += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    i++;
                    continue;
                }
                v.s += t[i++];
            }
            if (i >= t.size()) return fail("unterminated string");
            i++;
            return true;
        }
        if (c == '[') {
            i++;
            v.k = TomlValue::Arr;
            while (true) {
                skipAll();
                if (i < t.size() && t[i] == ']') {
                    i++;
                    return true;
                }
                TomlValue x;
                if (!value(x)) return false;
                v.arr.push_back(x);
                skipAll();
                if (i < t.size() && t[i] == ',') i++;
            }
        }
        if (c == '{') {
            i++;
            v.k = TomlValue::Table;
            while (true) {
                skipSpace();
                if (i < t.size() && t[i] == '}') {
                    i++;
                    return true;
                }
                std::string k;
                if (!key(k)) return false;
                skipSpace();
                if (i >= t.size() || t[i] != '=') return fail("expected '=' in an inline table");
                i++;
                TomlValue x;
                if (!value(x)) return false;
                v.table[k] = x;
                skipSpace();
                if (i < t.size() && t[i] == ',') i++;
            }
        }
        if (t.compare(i, 4, "true") == 0) {
            v.k = TomlValue::Bool;
            v.b = true;
            i += 4;
            return true;
        }
        if (t.compare(i, 5, "false") == 0) {
            v.k = TomlValue::Bool;
            i += 5;
            return true;
        }
        size_t s = i;
        while (i < t.size() && (std::isdigit((unsigned char)t[i]) || t[i] == '.' || t[i] == '-' || t[i] == '+' ||
                                t[i] == '_' || t[i] == 'e' || t[i] == 'E'))
            i++;
        if (s == i) return fail("expected a value");
        std::string num;
        for (size_t k = s; k < i; k++)
            if (t[k] != '_') num += t[k];
        v.k = TomlValue::Num;
        v.n = std::strtod(num.c_str(), nullptr);
        return true;
    }
    bool run(TomlDoc& doc) {
        std::string section;
        doc.sections[""];
        while (i < t.size()) {
            skipSpace();
            if (i >= t.size()) break;
            char c = t[i];
            if (c == '\n' || c == '\r') {
                if (c == '\n') line++;
                i++;
                continue;
            }
            if (c == '#') {
                skipComment();
                continue;
            }
            if (c == '[') {
                i++;
                size_t e = t.find(']', i);
                if (e == std::string::npos) return fail("expected ']'");
                section = t.substr(i, e - i);
                while (!section.empty() && section.front() == ' ') section.erase(0, 1);
                while (!section.empty() && section.back() == ' ') section.pop_back();
                i = e + 1;
                if (!doc.sections.count(section)) doc.sectionOrder.push_back(section);
                doc.sections[section];
                skipComment();
                continue;
            }
            std::string k;
            if (!key(k)) return false;
            skipSpace();
            if (i >= t.size() || t[i] != '=') return fail("expected '=' after '" + k + "'");
            i++;
            TomlValue v;
            if (!value(v)) return false;
            doc.sections[section][k] = v;
            skipComment();
            if (i < t.size() && t[i] != '\n' && t[i] != '\r') return fail("expected the end of the line");
        }
        return true;
    }
};

}  // namespace

const TomlValue* TomlDoc::get(const std::string& section, const std::string& key) const {
    auto s = sections.find(section);
    if (s == sections.end()) return nullptr;
    auto k = s->second.find(key);
    return k == s->second.end() ? nullptr : &k->second;
}

std::string TomlDoc::str(const std::string& section, const std::string& key, const std::string& def) const {
    const TomlValue* v = get(section, key);
    return v && v->k == TomlValue::Str ? v->s : def;
}

bool parseToml(const std::string& text, TomlDoc& out, std::string* error) {
    TomlParser p(text);
    bool ok = p.run(out);
    if (!ok && error) *error = p.err;
    return ok;
}

std::string tomlQuote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

static std::string S8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string findProjectRoot(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(std::u8string(dir.begin(), dir.end())), ec);
    for (int depth = 0; depth < 64 && !p.empty(); depth++) {
        if (vfsIsFile(S8(p / "ixo.toml"))) {
            auto u = p.u8string();
            return std::string(u.begin(), u.end());
        }
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return "";
}

}  // namespace ixo
