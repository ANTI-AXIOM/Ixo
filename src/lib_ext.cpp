// More of the standard library: the script's path, regular expressions (re),
// dates (time), CSV, running programs (os.exec) and calling C (ffi).
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <regex>
#include <unordered_map>

#include "builtins.hpp"
#include "common.hpp"
#include "process.hpp"
#include "vm.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ixo {

#define NATIVE(name) static Value name([[maybe_unused]] VM& vm, [[maybe_unused]] Value* a, [[maybe_unused]] int n)

namespace fs = std::filesystem;

static Value mapOf(VM& vm, std::initializer_list<std::pair<const char*, Value>> kv) {
    auto* m = vm.alloc<ObjMap>();
    for (auto& [k, v] : kv) m->set(vm.vstr(k), v);
    return Value::object(m);
}

// Byte offset -> character index (strings index by characters).
static int64_t charIndex(const std::string& s, size_t byteOff) {
    int64_t n = 0;
    for (size_t i = 0; i < byteOff && i < s.size(); i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

// ------------------------------------------------------------------ script path

void setScriptPath(VM& vm, const std::string& path) {
    int idx = vm.builtinIndex("os");
    if (idx < 0 || !isObjType(vm.builtins[idx].value, OT::Namespace)) return;
    auto* os = as<ObjNamespace>(vm.builtins[idx].value);
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(std::u8string(path.begin(), path.end())), ec);
    auto full = p.u8string();
    auto dir = p.parent_path().u8string();
    os->members["script"] = vm.vstr(std::string(full.begin(), full.end()));
    os->members["script_dir"] = vm.vstr(std::string(dir.begin(), dir.end()));
}

// ------------------------------------------------------------------ re

struct CompiledRe {
    std::regex re;
    int groups = 0;
};

// Compiled patterns, per thread (par tasks use their own).
static CompiledRe* compileRe(VM& vm, const std::string& pat, Value* errOut) {
    thread_local std::unordered_map<std::string, std::unique_ptr<CompiledRe>> cache;
    auto it = cache.find(pat);
    if (it != cache.end()) return it->second.get();
    if (cache.size() > 200) cache.clear();
    try {
        auto c = std::make_unique<CompiledRe>();
        c->re = std::regex(pat, std::regex::ECMAScript);
        c->groups = (int)c->re.mark_count();
        CompiledRe* raw = c.get();
        cache[pat] = std::move(c);
        return raw;
    } catch (const std::regex_error& e) {
        *errOut = vm.newError("BadPattern", "invalid regular expression '" + pat + "': " + e.what());
        return nullptr;
    }
}

static Value matchValue(VM& vm, const std::string& text, const std::smatch& m, size_t base) {
    std::vector<Value> groups;
    for (size_t g = 1; g < m.size(); g++) groups.push_back(m[g].matched ? vm.vstr(m[g].str()) : Value());
    size_t start = base + (size_t)m.position(0);
    return mapOf(vm, {{"text", vm.vstr(m[0].str())},
                      {"start", Value::integer(charIndex(text, start))},
                      {"end", Value::integer(charIndex(text, start + (size_t)m.length(0)))},
                      {"groups", vm.newList(std::move(groups))}});
}

NATIVE(re_test) {
    Value err;
    CompiledRe* c = compileRe(vm, vm.argStr(a, 0, "re.test")->s, &err);
    if (!c) return err;
    const std::string& s = vm.argStr(a, 1, "re.test")->s;
    return Value::boolean(std::regex_search(s, c->re));
}

NATIVE(re_find) {
    Value err;
    CompiledRe* c = compileRe(vm, vm.argStr(a, 0, "re.find")->s, &err);
    if (!c) return err;
    const std::string& s = vm.argStr(a, 1, "re.find")->s;
    std::smatch m;
    if (!std::regex_search(s, m, c->re)) return Value();
    return matchValue(vm, s, m, 0);
}

NATIVE(re_find_all) {
    Value err;
    CompiledRe* c = compileRe(vm, vm.argStr(a, 0, "re.find_all")->s, &err);
    if (!c) return err;
    const std::string& s = vm.argStr(a, 1, "re.find_all")->s;
    std::vector<Value> out;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), c->re); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        if (c->groups == 0) {
            out.push_back(vm.vstr(m[0].str()));
        } else if (c->groups == 1) {
            out.push_back(m[1].matched ? vm.vstr(m[1].str()) : Value());
        } else {
            std::vector<Value> g;
            for (size_t k = 1; k < m.size(); k++) g.push_back(m[k].matched ? vm.vstr(m[k].str()) : Value());
            out.push_back(vm.newTuple(std::move(g)));
        }
    }
    return vm.newList(std::move(out));
}

NATIVE(re_matches) {
    Value err;
    CompiledRe* c = compileRe(vm, vm.argStr(a, 0, "re.matches")->s, &err);
    if (!c) return err;
    const std::string& s = vm.argStr(a, 1, "re.matches")->s;
    std::vector<Value> out;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), c->re); it != std::sregex_iterator(); ++it)
        out.push_back(matchValue(vm, s, *it, 0));
    return vm.newList(std::move(out));
}

NATIVE(re_replace) {
    Value err;
    CompiledRe* c = compileRe(vm, vm.argStr(a, 0, "re.replace")->s, &err);
    if (!c) return err;
    const std::string& s = vm.argStr(a, 1, "re.replace")->s;
    if (isObjType(a[2], OT::String)) return vm.vstr(std::regex_replace(s, c->re, as<ObjString>(a[2])->s));
    // a function: called with each match's text (and groups), returns the replacement
    Value f = a[2];
    std::string out;
    size_t last = 0;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), c->re); it != std::sregex_iterator(); ++it) {
        const std::smatch& m = *it;
        out += s.substr(last, (size_t)m.position(0) - last);
        std::vector<Value> args{vm.vstr(m[0].str())};
        for (size_t k = 1; k < m.size(); k++) args.push_back(m[k].matched ? vm.vstr(m[k].str()) : Value());
        Value r = vm.callNested(f, args.data(), (int)args.size());
        out += vm.toStr(r);
        last = (size_t)m.position(0) + (size_t)m.length(0);
    }
    out += s.substr(last);
    return vm.vstr(out);
}

NATIVE(re_split) {
    Value err;
    CompiledRe* c = compileRe(vm, vm.argStr(a, 0, "re.split")->s, &err);
    if (!c) return err;
    const std::string& s = vm.argStr(a, 1, "re.split")->s;
    std::vector<Value> out;
    for (auto it = std::sregex_token_iterator(s.begin(), s.end(), c->re, -1); it != std::sregex_token_iterator(); ++it)
        out.push_back(vm.vstr(it->str()));
    return vm.newList(std::move(out));
}

NATIVE(re_escape) {
    const std::string& s = vm.argStr(a, 0, "re.escape")->s;
    std::string out;
    for (char c : s) {
        if (std::strchr("\\^$.|?*+()[]{}-/", c)) out += '\\';
        out += c;
    }
    return vm.vstr(out);
}

// ------------------------------------------------------------------ time

static double fromParts(int y, int mo, int d, int h, int mi, double s, bool utc) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = (int)std::floor(s);
    tm.tm_isdst = -1;
#ifdef _WIN32
    std::time_t t = utc ? _mkgmtime(&tm) : std::mktime(&tm);
#else
    std::time_t t = utc ? timegm(&tm) : std::mktime(&tm);
#endif
    return (double)t + (s - std::floor(s));
}

static std::tm partsOf(double epoch, bool utc) {
    std::time_t t = (std::time_t)std::floor(epoch);
    std::tm tm{};
#ifdef _WIN32
    if (utc) gmtime_s(&tm, &t);
    else localtime_s(&tm, &t);
#else
    if (utc) gmtime_r(&t, &tm);
    else localtime_r(&t, &tm);
#endif
    return tm;
}

static Value makeTime(VM& vm, double epoch) { return Value::object(vm.alloc<ObjTime>(epoch)); }

// time(2024, 5, 1, 12, 30, 0): a local date and time
NATIVE(time_make) {
    int y = (int)vm.argInt(a, 0, "time"), mo = n > 1 ? (int)vm.argInt(a, 1, "time") : 1, d = n > 2 ? (int)vm.argInt(a, 2, "time") : 1;
    int h = n > 3 ? (int)vm.argInt(a, 3, "time") : 0, mi = n > 4 ? (int)vm.argInt(a, 4, "time") : 0;
    double s = n > 5 ? vm.argNum(a, 5, "time") : 0;
    return makeTime(vm, fromParts(y, mo, d, h, mi, s, false));
}

NATIVE(time_utc) {
    int y = (int)vm.argInt(a, 0, "time.utc"), mo = n > 1 ? (int)vm.argInt(a, 1, "time.utc") : 1,
        d = n > 2 ? (int)vm.argInt(a, 2, "time.utc") : 1;
    int h = n > 3 ? (int)vm.argInt(a, 3, "time.utc") : 0, mi = n > 4 ? (int)vm.argInt(a, 4, "time.utc") : 0;
    double s = n > 5 ? vm.argNum(a, 5, "time.utc") : 0;
    return makeTime(vm, fromParts(y, mo, d, h, mi, s, true));
}

NATIVE(time_from_unix) { return makeTime(vm, vm.argNum(a, 0, "time.from_unix")); }
NATIVE(time_now) { return makeTime(vm, nowSeconds()); }

// Reads a date like the pattern (YYYY MM DD HH mm ss SSS), or ISO 8601 when
// there's no pattern: 2024-05-01, 2024-05-01 12:30, 2024-05-01T12:30:05Z,
// 2024-05-01T12:30:05+02:00.
NATIVE(time_parse) {
    const std::string& text = vm.argStr(a, 0, "time.parse")->s;
    auto bad = [&](const std::string& why) { return vm.newError("InvalidTime", "can't read '" + text + "' as a time: " + why); };
    int y = 1970, mo = 1, d = 1, h = 0, mi = 0;
    double s = 0;
    size_t i = 0;
    auto digits = [&](int count, int& out) {
        if (i + (size_t)count > text.size()) return false;
        int v = 0;
        for (int k = 0; k < count; k++) {
            char c = text[i + (size_t)k];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        i += (size_t)count;
        return true;
    };
    if (n > 1 && !a[1].isNil()) {
        const std::string& pat = vm.argStr(a, 1, "time.parse")->s;
        for (size_t p = 0; p < pat.size();) {
            auto starts = [&](const char* t) { return pat.compare(p, std::strlen(t), t) == 0; };
            int v = 0;
            if (starts("YYYY")) {
                if (!digits(4, y)) return bad("expected a 4-digit year");
                p += 4;
            } else if (starts("MM")) {
                if (!digits(2, mo)) return bad("expected a 2-digit month");
                p += 2;
            } else if (starts("DD")) {
                if (!digits(2, d)) return bad("expected a 2-digit day");
                p += 2;
            } else if (starts("HH")) {
                if (!digits(2, h)) return bad("expected a 2-digit hour");
                p += 2;
            } else if (starts("mm")) {
                if (!digits(2, mi)) return bad("expected 2-digit minutes");
                p += 2;
            } else if (starts("ss")) {
                if (!digits(2, v)) return bad("expected 2-digit seconds");
                s += v;
                p += 2;
            } else if (starts("SSS")) {
                if (!digits(3, v)) return bad("expected 3-digit milliseconds");
                s += v / 1000.0;
                p += 3;
            } else {
                if (i >= text.size() || text[i] != pat[p]) return bad(std::string("expected '") + pat[p] + "'");
                i++;
                p++;
            }
        }
        if (i != text.size()) return bad("unexpected text at the end");
        if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s >= 61) return bad("out of range");
        return makeTime(vm, fromParts(y, mo, d, h, mi, s, false));
    }
    // ISO 8601
    if (!digits(4, y) || i >= text.size() || text[i++] != '-' || !digits(2, mo) || i >= text.size() || text[i++] != '-' ||
        !digits(2, d))
        return bad("expected YYYY-MM-DD");
    bool utc = false;
    double offset = 0;
    if (i < text.size() && (text[i] == 'T' || text[i] == ' ')) {
        i++;
        if (!digits(2, h) || i >= text.size() || text[i++] != ':' || !digits(2, mi)) return bad("expected HH:mm");
        if (i < text.size() && text[i] == ':') {
            i++;
            int sec = 0;
            if (!digits(2, sec)) return bad("expected seconds");
            s = sec;
            if (i < text.size() && text[i] == '.') {
                i++;
                double frac = 0.1;
                while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                    s += (text[i] - '0') * frac;
                    frac /= 10;
                    i++;
                }
            }
        }
        if (i < text.size() && text[i] == 'Z') {
            utc = true;
            i++;
        } else if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            int sign = text[i] == '-' ? -1 : 1;
            i++;
            int oh = 0, om = 0;
            if (!digits(2, oh)) return bad("expected a time zone offset");
            if (i < text.size() && text[i] == ':') i++;
            if (i < text.size()) digits(2, om);
            utc = true;
            offset = sign * (oh * 3600.0 + om * 60.0);
        }
    }
    if (i != text.size()) return bad("unexpected text at the end");
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59) return bad("out of range");
    return makeTime(vm, fromParts(y, mo, d, h, mi, s, utc) - offset);
}

static double EP(Value v) { return as<ObjTime>(v)->epoch; }

NATIVE(tm_iso) {
    bool utc = n > 1 && vm.truthy(a[1]);
    double e = EP(a[0]);
    std::tm tm = partsOf(e, utc);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    std::string s = buf;
    if (utc) return vm.vstr(s + "Z");
    // local offset from UTC
    std::tm g = partsOf(e, true);
    long off = (long)std::lround(fromParts(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, true) -
                                 fromParts(g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, true));
    char z[32];
    snprintf(z, sizeof z, "%c%02ld:%02ld", off < 0 ? '-' : '+', std::labs(off) / 3600, (std::labs(off) % 3600) / 60);
    return vm.vstr(s + z);
}
NATIVE(tm_date) {
    std::tm tm = partsOf(EP(a[0]), false);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return vm.vstr(buf);
}
NATIVE(tm_diff) {
    if (!isObjType(a[1], OT::Time)) vm.panic("diff() needs another time, got " + vm.typeName(a[1]));
    return Value::number(EP(a[0]) - EP(a[1]));
}
NATIVE(tm_day_of_year) { return Value::integer(partsOf(EP(a[0]), false).tm_yday + 1); }
NATIVE(tm_start_of_day) {
    std::tm tm = partsOf(EP(a[0]), false);
    return makeTime(vm, fromParts(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, 0, 0, 0, false));
}
NATIVE(tm_add_days) {
    std::tm tm = partsOf(EP(a[0]), false);
    double frac = EP(a[0]) - std::floor(EP(a[0]));
    int64_t days = vm.argInt(a, 1, "add_days");
    return makeTime(vm, fromParts(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday + (int)days, tm.tm_hour, tm.tm_min,
                                  tm.tm_sec + frac, false));
}

// ------------------------------------------------------------------ csv

static std::vector<std::vector<std::string>> parseCsv(const std::string& t, char sep) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, any = false;
    for (size_t i = 0; i < t.size(); i++) {
        char c = t[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < t.size() && t[i + 1] == '"') {
                    field += '"';
                    i++;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == '"' && field.empty()) {
            quoted = true;
            any = true;
        } else if (c == sep) {
            row.push_back(field);
            field.clear();
            any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < t.size() && t[i + 1] == '\n') i++;
            if (any || !field.empty()) {
                row.push_back(field);
                rows.push_back(row);
            }
            row.clear();
            field.clear();
            any = false;
        } else {
            field += c;
            any = true;
        }
    }
    if (any || !field.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}

static char sepArg(VM& vm, Value* a, int n, int i) {
    if (n <= i || a[i].isNil()) return ',';
    const std::string& s = vm.argStr(a, i, "csv")->s;
    if (s.size() != 1) vm.panic("the CSV separator must be one character, got \"" + s + "\"");
    return s[0];
}

static Value csvValue(VM& vm, const std::string& text, bool header, char sep) {
    auto rows = parseCsv(text, sep);
    std::vector<Value> out;
    if (!header) {
        for (auto& r : rows) {
            std::vector<Value> cells;
            for (auto& c : r) cells.push_back(vm.vstr(c));
            out.push_back(vm.newList(std::move(cells)));
        }
        return vm.newList(std::move(out));
    }
    if (rows.empty()) return vm.newList();
    const auto& head = rows[0];
    for (size_t k = 1; k < rows.size(); k++) {
        auto* m = vm.alloc<ObjMap>();
        for (size_t c = 0; c < head.size(); c++) m->set(vm.vstr(head[c]), vm.vstr(c < rows[k].size() ? rows[k][c] : ""));
        out.push_back(Value::object(m));
    }
    return vm.newList(std::move(out));
}

NATIVE(csv_parse) {
    bool header = n > 1 ? vm.truthy(a[1]) : true;
    return csvValue(vm, vm.argStr(a, 0, "csv.parse")->s, header, sepArg(vm, a, n, 2));
}

static std::string csvCell(const std::string& s, char sep) {
    if (s.find_first_of(std::string("\"\r\n") + sep) == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += '"';
        o += c;
    }
    return o + "\"";
}

static std::string formatCsv(VM& vm, Value rowsV, char sep) {
    if (!isObjType(rowsV, OT::List)) vm.panic("csv needs a list of rows, got " + vm.typeName(rowsV));
    SeqView rows(*as<ObjList>(rowsV));
    std::string out;
    auto line = [&](const std::vector<std::string>& cells) {
        for (size_t i = 0; i < cells.size(); i++) out += (i ? std::string(1, sep) : "") + csvCell(cells[i], sep);
        out += "\n";
    };
    if (!rows.empty() && isObjType(rows[0], OT::Map)) {
        std::vector<Value> keys;
        for (auto& [k, v] : as<ObjMap>(rows[0])->entries)
            if (!k.isUndef()) keys.push_back(k);
        std::vector<std::string> head;
        for (auto& k : keys) head.push_back(vm.toStr(k));
        line(head);
        for (Value r : rows) {
            if (!isObjType(r, OT::Map)) vm.panic("every row must be a map when the first one is");
            std::vector<std::string> cells;
            for (auto& k : keys) {
                Value* v = as<ObjMap>(r)->find(k);
                cells.push_back(v && !v->isNil() ? vm.toStr(*v) : "");
            }
            line(cells);
        }
        return out;
    }
    for (Value r : rows) {
        std::vector<std::string> cells;
        SeqView items;
        if (!seqOf(r, items)) vm.panic("a CSV row must be a list, tuple or map, got " + vm.typeName(r));
        for (Value c : items) cells.push_back(c.isNil() ? "" : vm.toStr(c));
        line(cells);
    }
    return out;
}

NATIVE(csv_format) { return vm.vstr(formatCsv(vm, a[0], sepArg(vm, a, n, 1))); }

NATIVE(csv_read) {
    bool ok = false;
    std::string text = readFile(vm.argStr(a, 0, "csv.read")->s, &ok);
    if (!ok) return vm.newError("NotFound", "can't read " + as<ObjString>(a[0])->s);
    bool header = n > 1 ? vm.truthy(a[1]) : true;
    return csvValue(vm, text, header, sepArg(vm, a, n, 2));
}

NATIVE(csv_write) {
    std::string text = formatCsv(vm, a[1], sepArg(vm, a, n, 2));
    const std::string& path = vm.argStr(a, 0, "csv.write")->s;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return vm.newError("NotWritable", "can't write " + path);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return Value();
}

// ------------------------------------------------------------------ os.exec

NATIVE(os_exec) {
    std::vector<std::string> argv;
    for (Value v : SeqView(*vm.argList(a, 0, "os.exec"))) argv.push_back(vm.toStr(v));
    if (argv.empty()) vm.panic("os.exec() needs a command: os.exec([\"git\", \"status\"])");
    std::string input = n > 1 && !a[1].isNil() ? vm.toStr(a[1]) : "";
    std::string cwd = n > 2 && !a[2].isNil() ? vm.argStr(a, 2, "os.exec")->s : "";
    flushOut();
    RunResult r = runProcess(argv, input, cwd);
    if (!r.started) return vm.newError("NotFound", r.error);
    return mapOf(vm, {{"code", Value::integer(r.code)}, {"out", vm.vstr(r.out)}, {"err", vm.vstr(r.err)},
                      {"ok", Value::boolean(r.code == 0)}});
}

// ------------------------------------------------------------------ ffi

// A C function's signature: "int(int, double, str)".
struct FfiSig {
    enum T { Void, Int, UInt, I64, U64, Bool, Double, Float, Ptr, Str };
    T ret = Void;
    std::vector<T> args;
};

static bool parseFfiType(std::string s, FfiSig::T& t) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    static const std::unordered_map<std::string, FfiSig::T> names = {
        {"void", FfiSig::Void},   {"int", FfiSig::Int},     {"i32", FfiSig::Int},    {"uint", FfiSig::UInt},
        {"u32", FfiSig::UInt},    {"i64", FfiSig::I64},     {"long", FfiSig::I64},   {"u64", FfiSig::U64},
        {"size", FfiSig::U64},    {"bool", FfiSig::Bool},   {"double", FfiSig::Double}, {"f64", FfiSig::Double},
        {"float", FfiSig::Float}, {"f32", FfiSig::Float},   {"ptr", FfiSig::Ptr},    {"str", FfiSig::Str},
    };
    auto it = names.find(s);
    if (it == names.end()) return false;
    t = it->second;
    return true;
}

static bool parseFfiSig(const std::string& sig, FfiSig& out, std::string& err) {
    size_t open = sig.find('('), close = sig.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        err = "a signature looks like \"int(int, double, str)\"";
        return false;
    }
    if (!parseFfiType(sig.substr(0, open), out.ret)) {
        err = "unknown return type '" + sig.substr(0, open) + "'";
        return false;
    }
    std::string inner = sig.substr(open + 1, close - open - 1);
    size_t p = 0;
    while (p <= inner.size() && inner.find_first_not_of(' ') != std::string::npos) {
        size_t c = inner.find(',', p);
        std::string t = inner.substr(p, c == std::string::npos ? std::string::npos : c - p);
        FfiSig::T ty;
        if (!parseFfiType(t, ty) || ty == FfiSig::Void) {
            err = "unknown argument type '" + t + "'";
            return false;
        }
        if (ty == FfiSig::Float) {
            err = "float arguments aren't supported (use double)";
            return false;
        }
        out.args.push_back(ty);
        if (c == std::string::npos) break;
        p = c + 1;
    }
    if (out.args.size() > 6) {
        err = "at most 6 arguments";
        return false;
    }
    return true;
}

struct FfiArg {
    bool isDouble = false;
    int64_t i = 0;
    double d = 0;
};

// Calls fp with the arguments' C types (ints/pointers in integer registers,
// doubles in floating-point ones): one instantiation per combination.
template <class R, class... A>
static R ffiInvoke(void* fp, const std::vector<FfiArg>& args, A... done) {
    if constexpr (sizeof...(A) < 6) {
        if (sizeof...(A) < args.size()) {
            const FfiArg& x = args[sizeof...(A)];
            if (x.isDouble) return ffiInvoke<R, A..., double>(fp, args, done..., x.d);
            return ffiInvoke<R, A..., int64_t>(fp, args, done..., x.i);
        }
    }
    return reinterpret_cast<R (*)(A...)>(fp)(done...);
}

struct FfiFunc {
    void* fp;
    FfiSig sig;
    std::string name;
};

static std::mutex ffiMutex;
static std::vector<std::unique_ptr<FfiFunc>> ffiFuncs;
static std::vector<void*> ffiLibs;

NATIVE(ffi_call) {
    int64_t id = vm.nativeData.isInt() ? vm.nativeData.i : -1;
    FfiFunc* f;
    {
        std::lock_guard<std::mutex> lk(ffiMutex);
        if (id < 0 || id >= (int64_t)ffiFuncs.size()) vm.panic("internal: bad C function");
        f = ffiFuncs[(size_t)id].get();
    }
    if ((size_t)n != f->sig.args.size())
        vm.panic(f->name + "() takes " + std::to_string(f->sig.args.size()) + " arguments, got " + std::to_string(n));
    std::vector<FfiArg> args;
    std::vector<std::string> keep;  // C strings must live during the call
    keep.reserve((size_t)n);
    for (int i = 0; i < n; i++) {
        FfiArg x;
        switch (f->sig.args[(size_t)i]) {
            case FfiSig::Double:
                x.isDouble = true;
                x.d = vm.argNum(a, i, f->name.c_str());
                break;
            case FfiSig::Str:
                if (a[i].isNil()) {
                    x.i = 0;
                } else {
                    keep.push_back(vm.argStr(a, i, f->name.c_str())->s);
                    x.i = (int64_t)(uintptr_t)keep.back().c_str();
                }
                break;
            case FfiSig::Bool: x.i = vm.truthy(a[i]) ? 1 : 0; break;
            default:
                if (a[i].isNil()) x.i = 0;
                else x.i = vm.argInt(a, i, f->name.c_str());
                break;
        }
        args.push_back(x);
    }
    switch (f->sig.ret) {
        case FfiSig::Void: ffiInvoke<void>(f->fp, args); return Value();
        case FfiSig::Double: return Value::number(ffiInvoke<double>(f->fp, args));
        case FfiSig::Float: return Value::number((double)ffiInvoke<float>(f->fp, args));
        default: break;
    }
    int64_t r = ffiInvoke<int64_t>(f->fp, args);
    switch (f->sig.ret) {
        case FfiSig::Int: return Value::integer((int32_t)r);
        case FfiSig::UInt: return Value::integer((uint32_t)r);
        case FfiSig::Bool: return Value::boolean((r & 0xFF) != 0);
        case FfiSig::Str: return r ? vm.vstr((const char*)(uintptr_t)r) : Value();
        default: return Value::integer(r);
    }
}

NATIVE(ffi_load) {
    const std::string& path = vm.argStr(a, 0, "ffi.load")->s;
#ifdef _WIN32
    std::wstring w(path.begin(), path.end());
    int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    w.resize((size_t)std::max(len - 1, 0));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), len);
    void* h = (void*)LoadLibraryW(w.c_str());
#else
    void* h = dlopen(path.c_str(), RTLD_NOW);
#endif
    if (!h) return vm.newError("NotFound", "can't load the library '" + path + "'");
    std::lock_guard<std::mutex> lk(ffiMutex);
    ffiLibs.push_back(h);
    return Value::integer((int64_t)ffiLibs.size() - 1);
}

NATIVE(ffi_fn) {
    int64_t lib = vm.argInt(a, 0, "ffi.func");
    const std::string& name = vm.argStr(a, 1, "ffi.func")->s;
    const std::string& sigText = vm.argStr(a, 2, "ffi.func")->s;
    FfiSig sig;
    std::string err;
    if (!parseFfiSig(sigText, sig, err)) return vm.newError("BadSignature", "ffi.func(\"" + name + "\"): " + err);
    void* h;
    {
        std::lock_guard<std::mutex> lk(ffiMutex);
        if (lib < 0 || lib >= (int64_t)ffiLibs.size()) vm.panic("ffi.func() needs a library from ffi.load()");
        h = ffiLibs[(size_t)lib];
    }
#ifdef _WIN32
    void* fp = (void*)GetProcAddress((HMODULE)h, name.c_str());
#else
    void* fp = dlsym(h, name.c_str());
#endif
    if (!fp) return vm.newError("NotFound", "the library has no function '" + name + "'");
    int64_t id;
    {
        std::lock_guard<std::mutex> lk(ffiMutex);
        ffiFuncs.push_back(std::make_unique<FfiFunc>(FfiFunc{fp, sig, name}));
        id = (int64_t)ffiFuncs.size() - 1;
    }
    int arity = (int)sig.args.size();
    Value fn = vm.makeNative(name, ffi_call, arity, arity);
    as<ObjNative>(fn)->data = Value::integer(id);
    as<ObjNative>(fn)->path = "ffi:" + std::to_string(id);
    return fn;
}

// ------------------------------------------------------------------ registration

void registerExt(VM& vm) {
    auto* os = as<ObjNamespace>(vm.builtins[vm.builtinIndex("os")].value);
    vm.nsValue(os, "script", Value(), "str?", "The path of the program being run (nil in the REPL).");
    vm.nsValue(os, "script_dir", Value(), "str?", "The directory of the program being run.");
    vm.nsFunc(os, "exec", os_exec, 1, 3, "fn(list[str], str?, str?) -> map ! Error",
              "Runs a program (arguments as a list; optional input and working directory); returns "
              "{code, out, err, ok}.");

    auto* re = vm.defineNamespace("re", "Regular expressions (ECMAScript syntax).");
    vm.nativeModules.push_back("re");
    vm.nsFunc(re, "test", re_test, 2, 2, "fn(str, str) -> bool ! Error", "True if the pattern matches somewhere in the text.");
    vm.nsFunc(re, "find", re_find, 2, 2, "fn(str, str) -> map? ! Error",
              "The first match: {text, start, end, groups}, or nil.");
    vm.nsFunc(re, "find_all", re_find_all, 2, 2, "fn(str, str) -> list ! Error",
              "Every match's text (or its group, or a tuple of its groups).");
    vm.nsFunc(re, "matches", re_matches, 2, 2, "fn(str, str) -> list[map] ! Error",
              "Every match as {text, start, end, groups}.");
    vm.nsFunc(re, "replace", re_replace, 3, 3, "fn(str, str, any) -> str ! Error",
              "Replaces every match: with a string ($1 is a group) or what a function returns for it.");
    vm.nsFunc(re, "split", re_split, 2, 2, "fn(str, str) -> list[str] ! Error", "Splits the text where the pattern matches.");
    vm.nsFunc(re, "escape", re_escape, 1, 1, "fn(str) -> str", "The text with regex special characters escaped.");

    auto* tm = vm.defineNamespace("time", "Dates and times: time(2024, 5, 1, 12, 30) makes a local time.");
    vm.nativeModules.push_back("time");
    tm->callable = vm.makeNative("time", time_make, 1, 6);
    as<ObjNative>(tm->callable)->path = "c:time";
    vm.memberDocs["time"] = {"fn(int, int?, int?, int?, int?, num?) -> time", "A local date and time."};
    vm.nsFunc(tm, "utc", time_utc, 1, 6, "fn(int, int?, int?, int?, int?, num?) -> time", "A date and time in UTC.");
    vm.nsFunc(tm, "parse", time_parse, 1, 2, "fn(str, str?) -> time ! Error",
              "Reads a date: ISO 8601 (2024-05-01T12:30:00Z), or a pattern with YYYY MM DD HH mm ss SSS.");
    vm.nsFunc(tm, "from_unix", time_from_unix, 1, 1, "fn(num) -> time", "The time n seconds after 1970-01-01 UTC.");
    vm.nsFunc(tm, "now", time_now, 0, 0, "fn() -> time", "The current date and time.");
    Tag T_ = Tag::Time;
    vm.defineMethod(T_, "iso", tm_iso, 0, 1, "fn(bool?) -> str", "ISO 8601 text (in UTC with iso(true)).");
    vm.defineMethod(T_, "date", tm_date, 0, 0, "fn() -> str", "The date as YYYY-MM-DD.");
    vm.defineMethod(T_, "diff", tm_diff, 1, 1, "fn(time) -> float", "Seconds from another time to this one.");
    vm.defineMethod(T_, "day_of_year", tm_day_of_year, 0, 0, "fn() -> int", "1 to 366.");
    vm.defineMethod(T_, "start_of_day", tm_start_of_day, 0, 0, "fn() -> time", "Midnight of the same day.");
    vm.defineMethod(T_, "add_days", tm_add_days, 1, 1, "fn(int) -> time", "The same time of day n days later.");

    auto* csv = vm.defineNamespace("csv", "Comma-separated values.");
    vm.nativeModules.push_back("csv");
    vm.nsFunc(csv, "parse", csv_parse, 1, 3, "fn(str, bool?, str?) -> list",
              "Rows as maps keyed by the header (or lists with header false); optional separator.");
    vm.nsFunc(csv, "format", csv_format, 1, 2, "fn(list, str?) -> str", "CSV text from rows (lists, or maps with a header).");
    vm.nsFunc(csv, "read", csv_read, 1, 3, "fn(str, bool?, str?) -> list ! Error", "Reads and parses a CSV file.");
    vm.nsFunc(csv, "write", csv_write, 2, 3, "fn(str, list, str?) -> nil ! Error", "Writes rows to a CSV file.");

    auto* ffi = vm.defineNamespace("ffi", "Calling C functions in shared libraries (.dll, .so).");
    vm.nativeModules.push_back("ffi");
    vm.nsFunc(ffi, "load", ffi_load, 1, 1, "fn(str) -> int ! Error", "Loads a shared library; returns a handle.");
    vm.nsFunc(ffi, "func", ffi_fn, 3, 3, "fn(int, str, str) -> fn ! Error",
              "A C function as an Ixo function: ffi.func(lib, \"strlen\", \"u64(str)\"). Types: void int uint i64 u64 "
              "bool double float ptr str.");
}

}  // namespace ixo
