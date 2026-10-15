// Core builtins: printing, conversions, assertions, numbers, strings, errors.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>

#include "builtins.hpp"
#include "common.hpp"

namespace ixo {

#define NATIVE(name) static Value name([[maybe_unused]] VM& vm, [[maybe_unused]] Value* a, [[maybe_unused]] int n)

static std::string outBuf;
static std::mutex outMutex;  // par tasks print from several threads
static std::function<void(const std::string&)> outHook;
bool gStdinDisabled = false;

static void flushLocked() {
    if (!outBuf.empty()) {
        if (outHook)
            outHook(outBuf);
        else
            std::fwrite(outBuf.data(), 1, outBuf.size(), stdout);
        outBuf.clear();
    }
    if (!outHook) std::fflush(stdout);
}

void setOutputHook(std::function<void(const std::string&)> hook) {
    std::lock_guard<std::mutex> lk(outMutex);
    outHook = std::move(hook);
}

void writeOut(const std::string& s) {
    std::lock_guard<std::mutex> lk(outMutex);
    outBuf += s;
    if (outBuf.size() > 8192 || (outHook && s.find('\n') != std::string::npos)) flushLocked();
}

void flushOut() {
    std::lock_guard<std::mutex> lk(outMutex);
    flushLocked();
}

void registerAllBuiltins(VM& vm) {
    registerCore(vm);
    registerCollections(vm);
    registerMath(vm);
    registerIO(vm);
    registerConcurrency(vm);
    registerExt(vm);
    registerNet(vm);
    registerGpu(vm);
    registerArrays(vm);
}

// ------------------------------------------------------------------ globals

NATIVE(n_print) {
    std::string s;
    for (int i = 0; i < n; i++) {
        if (i) s += ' ';
        s += vm.toStr(a[i]);
    }
    s += '\n';
    writeOut(s);
    return Value();
}

NATIVE(n_write) {
    std::string s;
    for (int i = 0; i < n; i++) s += vm.toStr(a[i]);
    writeOut(s);
    return Value();
}

NATIVE(n_input) {
    if (n > 0) writeOut(vm.toStr(a[0]));
    flushOut();
    std::string line;
    if (gStdinDisabled || !std::getline(std::cin, line)) return Value();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return vm.vstr(line);
}

static Value parseNumber(VM& vm, const std::string& raw, bool wantInt, bool wantFloat) {
    // the common case (plain digits, a sign, a point, an exponent): no copies
    if (!raw.empty() && raw[0] != '+' && raw.find_first_of("_ \t\r\n\v\fxXbB") == std::string::npos) {
        const char* b = raw.data();
        const char* e = b + raw.size();
        if (!wantFloat) {
            int64_t iv;
            auto r = std::from_chars(b, e, iv);
            if (r.ec == std::errc() && r.ptr == e) return Value::integer(iv);
        }
        double d;
        auto r = std::from_chars(b, e, d);
        if (r.ec == std::errc() && r.ptr == e && !wantInt) return Value::number(d);
    }
    std::string s;
    for (char c : raw)
        if (c != '_') s += c;
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t st = 0;
    while (st < s.size() && std::isspace((unsigned char)s[st])) st++;
    s = s.substr(st);
    if (s.empty()) return vm.newError("InvalidNumber", "empty string is not a number");
    const char* c = s.c_str();
    char* end = nullptr;
    bool neg = s[0] == '-';
    std::string body = (s[0] == '-' || s[0] == '+') ? s.substr(1) : s;
    if (!wantFloat && body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'b')) {
        errno = 0;
        unsigned long long v = std::strtoull(body.c_str() + 2, &end, body[1] == 'x' ? 16 : 2);
        if (*end == '\0' && errno == 0) return Value::integer(neg ? -(int64_t)v : (int64_t)v);
        return vm.newError("InvalidNumber", "'" + raw + "' is not a number");
    }
    errno = 0;
    long long iv = std::strtoll(c, &end, 10);
    if (*end == '\0' && errno == 0 && !wantFloat) return Value::integer(iv);
    double d = std::strtod(c, &end);
    if (*end != '\0') return vm.newError("InvalidNumber", "'" + raw + "' is not a number");
    if (wantInt) {
        if (d != std::floor(d)) return vm.newError("InvalidNumber", "'" + raw + "' is not a whole number");
        return Value::integer((int64_t)d);
    }
    return Value::number(d);
}

NATIVE(n_str) { return isObjType(a[0], OT::String) ? a[0] : vm.vstr(vm.toStr(a[0])); }

NATIVE(n_int) {
    Value v = a[0];
    if (v.isInt()) return v;
    if (v.isFloat()) {
        if (!std::isfinite(v.f)) return vm.newError("InvalidNumber", "can't convert " + vm.toStr(v) + " to int");
        return Value::integer((int64_t)std::trunc(v.f));
    }
    if (v.isBool()) return Value::integer(v.b ? 1 : 0);
    if (isObjType(v, OT::String)) return parseNumber(vm, as<ObjString>(v)->s, true, false);
    vm.panic("can't convert a " + vm.typeName(v) + " to int");
}

NATIVE(n_float) {
    Value v = a[0];
    if (v.isNum()) return Value::number(v.num());
    if (v.isBool()) return Value::number(v.b ? 1 : 0);
    if (isObjType(v, OT::String)) return parseNumber(vm, as<ObjString>(v)->s, false, true);
    vm.panic("can't convert a " + vm.typeName(v) + " to float");
}

NATIVE(n_bool) { return Value::boolean(vm.truthy(a[0])); }
NATIVE(n_type) { return vm.vstr(vm.typeName(a[0])); }

NATIVE(n_error) {
    std::string msg = n > 0 ? vm.toStr(a[0]) : "error";
    std::string kind = n > 1 ? vm.toStr(a[1]) : "Error";
    return vm.newError(kind, msg);
}

NATIVE(n_is_error) { return Value::boolean(isObjType(a[0], OT::Error)); }

NATIVE(n_panic) { vm.panic(n > 0 ? vm.toStr(a[0]) : "panic"); }

NATIVE(n_assert) {
    if (!vm.truthy(a[0])) vm.panic("assertion failed" + (n > 1 ? ": " + vm.toStr(a[1]) : std::string()));
    return Value();
}

// Where two unequal values first differ ("" when that says nothing more than a != b).
static std::string firstDifference(VM& vm, Value a, Value b, const std::string& path, int depth) {
    auto at = [&](const std::string& p) { return p.empty() ? std::string("") : " at " + p; };
    auto clip = [](std::string s) { return s.size() > 120 ? s.substr(0, 117) + "..." : s; };
    if (depth > 20) return "";
    if (isObjType(a, OT::String) && isObjType(b, OT::String)) {
        const std::string& x = as<ObjString>(a)->s;
        const std::string& y = as<ObjString>(b)->s;
        if (x.find('\n') == std::string::npos && y.find('\n') == std::string::npos && x.size() < 40 && y.size() < 40)
            return path.empty() ? "" : "\n  first difference" + at(path) + ": " + vm.repr(a) + " vs " + vm.repr(b);
        size_t line = 1, col = 1, i = 0;
        while (i < x.size() && i < y.size() && x[i] == y[i]) {
            if (x[i] == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
            i++;
        }
        auto lineAt = [](const std::string& s, size_t pos) {
            size_t st = s.rfind('\n', pos == 0 ? 0 : pos - 1);
            st = (st == std::string::npos || pos == 0) ? 0 : st + 1;
            if (pos > 0 && pos <= s.size() && s[pos - 1] == '\n') st = pos;
            size_t en = s.find('\n', pos);
            return s.substr(st, en == std::string::npos ? std::string::npos : en - st);
        };
        return "\n  strings differ" + at(path) + " on line " + std::to_string(line) + ", column " + std::to_string(col) +
               ":\n    left:  " + clip(lineAt(x, i)) + "\n    right: " + clip(lineAt(y, i));
    }
    auto seq = [&](SeqView x, SeqView y) -> std::string {
        for (size_t i = 0; i < x.size() && i < y.size(); i++)
            if (!vm.equals(x[i], y[i])) {
                std::string p = path + "[" + std::to_string(i) + "]";
                std::string deeper = firstDifference(vm, x[i], y[i], p, depth + 1);
                if (!deeper.empty()) return deeper;
                return "\n  first difference at " + p + ": " + clip(vm.repr(x[i])) + " vs " + clip(vm.repr(y[i]));
            }
        if (x.size() != y.size())
            return "\n  lengths differ" + at(path) + ": " + std::to_string(x.size()) + " vs " + std::to_string(y.size()) +
                   (x.size() > y.size() ? " (left has extra " + clip(vm.repr(x[y.size()])) + ")"
                                        : " (right has extra " + clip(vm.repr(y[x.size()])) + ")");
        return "";
    };
    if (isObjType(a, OT::List) && isObjType(b, OT::List)) return seq(SeqView(*as<ObjList>(a)), SeqView(*as<ObjList>(b)));
    if (isObjType(a, OT::Tuple) && isObjType(b, OT::Tuple)) return seq(as<ObjTuple>(a)->items, as<ObjTuple>(b)->items);
    if (isObjType(a, OT::Map) && isObjType(b, OT::Map)) {
        auto* x = as<ObjMap>(a);
        auto* y = as<ObjMap>(b);
        for (auto& [k, v] : x->entries) {
            if (k.isUndef()) continue;
            Value* w = y->find(k);
            std::string p = path + "[" + vm.repr(k) + "]";
            if (!w) return "\n  key " + clip(vm.repr(k)) + " is only on the left" + at(path);
            if (!vm.equals(v, *w)) {
                std::string deeper = firstDifference(vm, v, *w, p, depth + 1);
                if (!deeper.empty()) return deeper;
                return "\n  first difference at " + p + ": " + clip(vm.repr(v)) + " vs " + clip(vm.repr(*w));
            }
        }
        for (auto& [k, v] : y->entries)
            if (!k.isUndef() && !x->find(k)) return "\n  key " + clip(vm.repr(k)) + " is only on the right" + at(path);
        return "";
    }
    if (isObjType(a, OT::Instance) && isObjType(b, OT::Instance) && as<ObjInstance>(a)->cls == as<ObjInstance>(b)->cls) {
        auto* x = as<ObjInstance>(a);
        auto* y = as<ObjInstance>(b);
        for (size_t i = 0; i < x->fields.size(); i++)
            if (!vm.equals(x->fields[i], y->fields[i])) {
                std::string p = path + "." + (i < x->cls->fields.size() ? x->cls->fields[i] : "?");
                std::string deeper = firstDifference(vm, x->fields[i], y->fields[i], p, depth + 1);
                if (!deeper.empty()) return deeper;
                return "\n  first difference at " + p + ": " + clip(vm.repr(x->fields[i])) + " vs " +
                       clip(vm.repr(y->fields[i]));
            }
        return "";
    }
    if (!path.empty()) return "\n  first difference at " + path + ": " + clip(vm.repr(a)) + " vs " + clip(vm.repr(b));
    return "";
}

NATIVE(n_assert_eq) {
    if (!vm.equals(a[0], a[1])) {
        std::string msg = "assertion failed: " + vm.repr(a[0]) + " != " + vm.repr(a[1]);
        if (n > 2) msg += " (" + vm.toStr(a[2]) + ")";
        msg += firstDifference(vm, a[0], a[1], "", 0);
        vm.panic(msg);
    }
    return Value();
}

// fn main(input: str, top: int = 10, upper: bool = false): the command line parsed
// from main's parameters. spec is [doc, name, kind, has default, name, kind, ...];
// gives the value of parameter i (undef: its default).
NATIVE(n_main_args) {
    auto* spec = as<ObjList>(a[0]);
    size_t want = (size_t)a[1].i;
    struct P {
        std::string name, kind;
        bool hasDef;
    };
    std::vector<P> ps;
    for (size_t i = 1; i + 2 < spec->len(); i += 3)
        ps.push_back({vm.toStr(spec->get(i)), vm.toStr(spec->get(i + 1)), spec->get(i + 2).isBool() && spec->get(i + 2).b});
    std::string doc = vm.toStr(spec->get(0));
    std::vector<std::string> args;
    int ai = vm.builtinIndex("args");
    if (ai >= 0 && isObjType(vm.builtins[ai].value, OT::List)) {
        auto* l = as<ObjList>(vm.builtins[ai].value);
        for (size_t i = 0; i < l->len(); i++) args.push_back(vm.toStr(l->get(i)));
    }
    std::string prog = "program";
    int osi = vm.builtinIndex("os");
    if (osi >= 0 && isObjType(vm.builtins[osi].value, OT::Namespace)) {
        auto& mem = as<ObjNamespace>(vm.builtins[osi].value)->members;
        auto sc = mem.find("script");
        if (sc != mem.end() && isObjType(sc->second, OT::String)) {
            prog = as<ObjString>(sc->second)->s;
            size_t sl = prog.find_last_of("/\\");
            if (sl != std::string::npos) prog = prog.substr(sl + 1);
        }
    }
    auto dashed = [](std::string s) {
        for (auto& c : s)
            if (c == '_') c = '-';
        return s;
    };
    auto isList = [](const P& p) { return p.kind.rfind("list", 0) == 0; };
    auto elemKind = [&](const P& p) {
        if (!isList(p)) return p.kind;
        size_t lb = p.kind.find('['), rb = p.kind.rfind(']');
        return lb == std::string::npos ? std::string("str") : p.kind.substr(lb + 1, rb - lb - 1);
    };
    std::string usage = "usage: " + prog;
    for (auto& p : ps) {
        std::string up = p.name;
        for (auto& c : up) c = (char)std::toupper((unsigned char)c);
        if (p.kind == "bool")
            usage += " [--" + dashed(p.name) + "]";
        else if (isList(p))
            usage += p.hasDef ? " [" + p.name + "...]" : " " + p.name + "...";
        else if (p.hasDef)
            usage += " [--" + dashed(p.name) + " " + up + "]";
        else
            usage += " " + p.name;
    }
    auto fail = [&](const std::string& msg) {
        flushOut();
        std::fprintf(stderr, "error: %s\n%s\n", msg.c_str(), usage.c_str());
        std::fflush(stderr);
        hardExit(2);
    };
    auto convert = [&](const std::string& kind, const std::string& s, const std::string& what) -> Value {
        if (kind == "int") {
            int64_t v = 0;
            auto r = std::from_chars(s.data(), s.data() + s.size(), v);
            if (r.ec != std::errc() || r.ptr != s.data() + s.size()) fail(what + " should be a whole number, not '" + s + "'");
            return Value::integer(v);
        }
        if (kind == "float" || kind == "num") {
            char* end = nullptr;
            double v = std::strtod(s.c_str(), &end);
            if (s.empty() || *end) fail(what + " should be a number, not '" + s + "'");
            return Value::number(v);
        }
        if (kind == "bool") {
            if (s == "true" || s == "yes" || s == "1") return Value::boolean(true);
            if (s == "false" || s == "no" || s == "0") return Value::boolean(false);
            fail(what + " should be true or false, not '" + s + "'");
        }
        return vm.vstr(s);
    };
    auto find = [&](const std::string& key) -> int {
        for (size_t i = 0; i < ps.size(); i++)
            if (ps[i].name == key) return (int)i;
        return -1;
    };
    std::vector<Value> vals(ps.size(), Value::undef());
    std::vector<bool> given(ps.size(), false);
    std::vector<std::vector<Value>> lists(ps.size());
    std::vector<std::string> positional;
    bool optionsDone = false;
    for (size_t k = 0; k < args.size(); k++) {
        const std::string& s = args[k];
        if (!optionsDone && s == "--") {
            optionsDone = true;
            continue;
        }
        if (!optionsDone && (s == "-h" || s == "--help")) {
            flushOut();
            if (!doc.empty()) std::printf("%s\n\n", doc.c_str());
            std::printf("%s\n", usage.c_str());
            bool head = false;
            for (auto& p : ps) {
                if (!p.hasDef && p.kind != "bool") continue;
                if (!head) std::printf("\noptions:\n");
                head = true;
                std::printf("  --%s%s\n", dashed(p.name).c_str(), p.kind == "bool" ? "" : (" (" + p.kind + ")").c_str());
            }
            std::fflush(stdout);
            hardExit(0);
        }
        if (!optionsDone && s.size() > 2 && s.compare(0, 2, "--") == 0) {
            std::string key = s.substr(2), val;
            bool hasVal = false;
            size_t eq = key.find('=');
            if (eq != std::string::npos) {
                val = key.substr(eq + 1);
                key = key.substr(0, eq);
                hasVal = true;
            }
            for (auto& c : key)
                if (c == '-') c = '_';
            bool neg = false;
            int pi = find(key);
            if (pi < 0 && key.rfind("no_", 0) == 0 && (pi = find(key.substr(3))) >= 0) {
                neg = true;
                if (ps[(size_t)pi].kind != "bool") pi = -1;
            }
            if (pi < 0) fail("unknown option " + s.substr(0, eq == std::string::npos ? s.size() : eq + 2));
            P& p = ps[(size_t)pi];
            given[(size_t)pi] = true;
            if (p.kind == "bool" && !hasVal) {
                vals[(size_t)pi] = Value::boolean(!neg);
                continue;
            }
            if (!hasVal) {
                if (k + 1 >= args.size()) fail("--" + dashed(p.name) + " needs a value");
                val = args[++k];
            }
            if (isList(p))
                lists[(size_t)pi].push_back(convert(elemKind(p), val, "--" + dashed(p.name)));
            else
                vals[(size_t)pi] = convert(p.kind, val, "--" + dashed(p.name));
            continue;
        }
        positional.push_back(s);
    }
    size_t at = 0;
    for (size_t pi = 0; pi < ps.size(); pi++) {
        P& p = ps[pi];
        if (isList(p)) {
            while (at < positional.size()) lists[pi].push_back(convert(elemKind(p), positional[at++], p.name));
            if (lists[pi].empty() && !p.hasDef) fail("missing " + p.name);
            if (!lists[pi].empty()) vals[pi] = vm.newList(lists[pi]);
            continue;
        }
        if (given[pi] || p.hasDef || p.kind == "bool") continue;
        if (at >= positional.size()) fail("missing " + p.name);
        vals[pi] = convert(p.kind, positional[at++], p.name);
    }
    if (at < positional.size()) fail("unexpected argument '" + positional[at] + "'");
    return want < vals.size() ? vals[want] : Value::undef();
}

NATIVE(n_exit) {
    flushOut();
    std::fflush(stderr);
    hardExit(n > 0 ? (int)vm.argInt(a, 0, "exit") : 0);
}

NATIVE(n_range) {
    if (n == 1) return makeRange(vm, Value::integer(0), a[0], Value::integer(1), false);
    return makeRange(vm, a[0], a[1], n > 2 ? a[2] : Value::integer(1), false);
}

NATIVE(n_freeze) { return vm.freeze(a[0]); }
NATIVE(n_frozen) { return Value::boolean(a[0].isObj() && a[0].o->frozen); }

NATIVE(n_memory) { return Value::integer((int64_t)vm.bytesAllocated); }
NATIVE(n_gc) { return Value::boolean(vm.collectNow()); }

NATIVE(n_ord) {
    const std::string& s = vm.argStr(a, 0, "ord")->s;
    if (s.empty()) vm.panic("ord() needs a non-empty string");
    unsigned char c = s[0];
    uint32_t cp = c;
    int extra = 0;
    if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
    else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
    else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
    for (int i = 1; i <= extra && i < (int)s.size(); i++) cp = (cp << 6) | (s[i] & 0x3F);
    return Value::integer(cp);
}

NATIVE(n_chr) {
    int64_t cp = vm.argInt(a, 0, "chr");
    std::string s;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    return vm.vstr(s);
}

// ------------------------------------------------------------------ numbers

static double num0(VM& vm, Value* a, const char* f) { return vm.argNum(a, 0, f); }

static Value intOrFloat(double d) {
    if (std::isfinite(d) && std::fabs(d) < 9.2e18) return Value::integer((int64_t)d);
    return Value::number(d);
}

NATIVE(m_abs) { return a[0].isInt() ? Value::integer(a[0].i < 0 ? -a[0].i : a[0].i) : Value::number(std::fabs(a[0].f)); }
NATIVE(m_sqrt) { return Value::number(std::sqrt(num0(vm, a, "sqrt"))); }
NATIVE(m_root) {
    double k = vm.argNum(a, 1, "root");
    if (k == 0) vm.panic("the 0th root is undefined");
    double x = a[0].isInt() ? (double)a[0].i : a[0].f;
    double res = k == 2 ? std::sqrt(x)
                 : k == 3 ? std::cbrt(x)
                 : (x < 0 && std::fmod(k, 2) == 1) ? -std::pow(-x, 1.0 / k)
                                                    : std::pow(x, 1.0 / k);
    if (a[0].isInt() && a[1].isInt() && std::fabs(res - std::round(res)) < 1e-9 && std::fabs(res) < 9e18)
        return Value::integer((int64_t)std::llround(res));  // 27.root(3) is 3
    return Value::number(res);
}
NATIVE(m_pow) {
    if (a[0].isInt() && a[1].isInt() && a[1].i >= 0) {
        uint64_t r = 1, b = (uint64_t)a[0].i;
        int64_t e = a[1].i;
        while (e > 0) {
            if (e & 1) r *= b;
            b *= b;
            e >>= 1;
        }
        return Value::integer((int64_t)r);
    }
    return Value::number(std::pow(num0(vm, a, "pow"), vm.argNum(a, 1, "pow")));
}
NATIVE(m_sin) { return Value::number(std::sin(num0(vm, a, "sin"))); }
NATIVE(m_cos) { return Value::number(std::cos(num0(vm, a, "cos"))); }
NATIVE(m_tan) { return Value::number(std::tan(num0(vm, a, "tan"))); }
NATIVE(m_asin) { return Value::number(std::asin(num0(vm, a, "asin"))); }
NATIVE(m_acos) { return Value::number(std::acos(num0(vm, a, "acos"))); }
NATIVE(m_atan) {
    if (n > 1) return Value::number(std::atan2(num0(vm, a, "atan"), vm.argNum(a, 1, "atan")));
    return Value::number(std::atan(num0(vm, a, "atan")));
}
NATIVE(m_ln) { return Value::number(std::log(num0(vm, a, "ln"))); }
NATIVE(m_log) {
    double x = num0(vm, a, "log");
    if (n > 1) return Value::number(std::log(x) / std::log(vm.argNum(a, 1, "log")));
    return Value::number(std::log10(x));
}
NATIVE(m_exp) { return Value::number(std::exp(num0(vm, a, "exp"))); }
NATIVE(m_floor) { return a[0].isInt() ? a[0] : intOrFloat(std::floor(a[0].f)); }
NATIVE(m_ceil) { return a[0].isInt() ? a[0] : intOrFloat(std::ceil(a[0].f)); }
NATIVE(m_round) {
    if (n > 1) {
        int64_t digits = vm.argInt(a, 1, "round");
        double p = std::pow(10.0, (double)digits);
        return Value::number(std::round(a[0].num() * p) / p);
    }
    return a[0].isInt() ? a[0] : intOrFloat(std::round(a[0].f));
}
NATIVE(m_int) { return a[0].isInt() ? a[0] : intOrFloat(std::trunc(a[0].f)); }
NATIVE(m_float) { return Value::number(a[0].num()); }
NATIVE(m_clamp) {
    Value lo = a[1], hi = a[2];
    if (vm.compare(a[0], lo) < 0) return lo;
    if (vm.compare(a[0], hi) > 0) return hi;
    return a[0];
}
NATIVE(m_lerp) {
    double x = num0(vm, a, "lerp"), y = vm.argNum(a, 1, "lerp"), t = vm.argNum(a, 2, "lerp");
    return Value::number(x + (y - x) * t);
}
NATIVE(m_min) {
    if (n == 1) vm.panic("min() needs at least 2 values (or call .min on a list)");
    Value best = a[0];
    for (int i = 1; i < n; i++)
        if (vm.compare(a[i], best) < 0) best = a[i];
    return best;
}
NATIVE(m_max) {
    if (n == 1) vm.panic("max() needs at least 2 values (or call .max on a list)");
    Value best = a[0];
    for (int i = 1; i < n; i++)
        if (vm.compare(a[i], best) > 0) best = a[i];
    return best;
}
NATIVE(m_sign) {
    double d = a[0].num();
    int s = d > 0 ? 1 : d < 0 ? -1 : 0;
    return a[0].isInt() ? Value::integer(s) : Value::number(s);
}
NATIVE(m_even) { return Value::boolean(vm.argInt(a, 0, "even") % 2 == 0); }
NATIVE(m_odd) { return Value::boolean(vm.argInt(a, 0, "odd") % 2 != 0); }
NATIVE(m_is_nan) { return Value::boolean(a[0].isFloat() && std::isnan(a[0].f)); }
NATIVE(m_wait) {
    double ms = num0(vm, a, "wait");
    vm.sleepCurrent(ms, true);
    return Value();
}
NATIVE(m_hex) {
    char buf[32];
    snprintf(buf, sizeof buf, "%llx", (unsigned long long)vm.argInt(a, 0, "hex"));
    return vm.vstr(buf);
}

// ------------------------------------------------------------------ strings

static const std::string& S(Value v) { return as<ObjString>(v)->s; }

NATIVE(s_len) {
    auto* s = as<ObjString>(a[0]);
    return Value::integer((int64_t)(VM::ascii(s) ? s->s.size() : utf8Length(s->s)));
}
NATIVE(s_up) {
    std::string s = S(a[0]);
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return vm.vstr(s);
}
NATIVE(s_down) {
    std::string s = S(a[0]);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return vm.vstr(s);
}
static std::string trimWith(const std::string& s, bool left, bool right) {
    size_t b = 0, e = s.size();
    if (left)
        while (b < e && std::isspace((unsigned char)s[b])) b++;
    if (right)
        while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}
NATIVE(s_trim) { return vm.vstr(trimWith(S(a[0]), true, true)); }
NATIVE(s_trim_start) { return vm.vstr(trimWith(S(a[0]), true, false)); }
NATIVE(s_trim_end) { return vm.vstr(trimWith(S(a[0]), false, true)); }
NATIVE(s_cut) {
    const std::string& s = S(a[0]);
    std::vector<Value> out;
    if (n < 2 || (isObjType(a[1], OT::String) && S(a[1]).empty() && false)) {
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
            size_t st = i;
            while (i < s.size() && !std::isspace((unsigned char)s[i])) i++;
            if (i > st) out.push_back(vm.vstr(s.substr(st, i - st)));
        }
        return vm.newList(std::move(out));
    }
    const std::string& sep = vm.argStr(a, 1, "cut")->s;
    if (sep.empty()) {
        for (size_t i = 0; i < s.size();) {
            size_t len = 1;
            unsigned char c = s[i];
            if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
            out.push_back(vm.vstr(s.substr(i, len)));
            i += len;
        }
        return vm.newList(std::move(out));
    }
    size_t pieces = 1;  // one allocation for the list's items
    for (size_t p = s.find(sep); p != std::string::npos; p = s.find(sep, p + sep.size())) pieces++;
    out.reserve(pieces);
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(vm.vstr(s.substr(start)));
            break;
        }
        out.push_back(vm.vstr(s.substr(start, p - start)));
        start = p + sep.size();
    }
    return vm.newList(std::move(out));
}
NATIVE(s_join) {
    ObjList* l = vm.argList(a, 1, "join");
    std::string out;
    size_t n2 = l->len();
    for (size_t i = 0; i < n2; i++) {  // any kind, without widening
        if (i) out += S(a[0]);
        out += vm.toStr(l->get(i));
    }
    return vm.vstr(out);
}
NATIVE(s_has) { return Value::boolean(S(a[0]).find(vm.argStr(a, 1, "has")->s) != std::string::npos); }
NATIVE(s_starts) {
    const std::string& s = S(a[0]);
    const std::string& p = vm.argStr(a, 1, "starts")->s;
    return Value::boolean(s.compare(0, p.size(), p) == 0 && s.size() >= p.size());
}
NATIVE(s_ends) {
    const std::string& s = S(a[0]);
    const std::string& p = vm.argStr(a, 1, "ends")->s;
    return Value::boolean(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0);
}
NATIVE(s_find) {
    const std::string& s = S(a[0]);
    size_t p = s.find(vm.argStr(a, 1, "find")->s);
    if (p == std::string::npos) return Value();
    if (VM::ascii(as<ObjString>(a[0]))) return Value::integer((int64_t)p);
    return Value::integer((int64_t)utf8Length(s.substr(0, p)));
}
NATIVE(s_count) {
    const std::string& s = S(a[0]);
    const std::string& sub = vm.argStr(a, 1, "count")->s;
    if (sub.empty()) return Value::integer(0);
    int64_t c = 0;
    for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + sub.size())) c++;
    return Value::integer(c);
}
NATIVE(s_replace) {
    std::string s = S(a[0]);
    const std::string& from = vm.argStr(a, 1, "replace")->s;
    const std::string& to = vm.argStr(a, 2, "replace")->s;
    if (from.empty()) return a[0];
    std::string out;
    size_t start = 0;
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, start)) {
        out += s.substr(start, p - start) + to;
        start = p + from.size();
    }
    out += s.substr(start);
    return vm.vstr(out);
}
NATIVE(s_repeat) {
    int64_t k = vm.argInt(a, 1, "repeat");
    std::string out;
    for (int64_t i = 0; i < k; i++) out += S(a[0]);
    return vm.vstr(out);
}
NATIVE(s_rev) {
    const std::string& s = S(a[0]);
    std::vector<std::string> chars;
    for (size_t i = 0; i < s.size();) {
        size_t len = 1;
        unsigned char c = s[i];
        if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
        chars.push_back(s.substr(i, len));
        i += len;
    }
    std::string out;
    for (auto it = chars.rbegin(); it != chars.rend(); ++it) out += *it;
    return vm.vstr(out);
}
NATIVE(s_lines) {
    const std::string& s = S(a[0]);
    std::vector<Value> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find('\n', start);
        std::string line = s.substr(start, p == std::string::npos ? std::string::npos : p - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (p == std::string::npos) {
            if (!line.empty()) out.push_back(vm.vstr(line));
            break;
        }
        out.push_back(vm.vstr(line));
        start = p + 1;
    }
    return vm.newList(std::move(out));
}
NATIVE(s_chars) {
    Value sep = vm.vstr("");
    Value args[2] = {a[0], sep};
    return s_cut(vm, args, 2);
}
NATIVE(s_bytes) {
    std::vector<Value> out;
    for (unsigned char c : S(a[0])) out.push_back(Value::integer(c));
    return vm.newList(std::move(out));
}
NATIVE(s_num) { return parseNumber(vm, S(a[0]), false, false); }
NATIVE(s_int) { return parseNumber(vm, S(a[0]), true, false); }
NATIVE(s_float) { return parseNumber(vm, S(a[0]), false, true); }
NATIVE(s_is_empty) { return Value::boolean(S(a[0]).empty()); }
NATIVE(s_is_digit) {
    const std::string& s = S(a[0]);
    return Value::boolean(!s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c); }));
}
NATIVE(s_is_alpha) {
    const std::string& s = S(a[0]);
    return Value::boolean(!s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isalpha((unsigned char)c); }));
}
NATIVE(s_is_space) {
    const std::string& s = S(a[0]);
    return Value::boolean(!s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isspace((unsigned char)c); }));
}
NATIVE(s_slice) {
    const std::string& s = S(a[0]);
    bool asc = VM::ascii(as<ObjString>(a[0]));
    int64_t len = (int64_t)(asc ? s.size() : utf8Length(s));
    int64_t lo = vm.argInt(a, 1, "slice");
    int64_t hi = n > 2 ? vm.argInt(a, 2, "slice") : len;
    if (lo < 0) lo += len;
    if (hi < 0) hi += len;
    lo = std::clamp<int64_t>(lo, 0, len);
    hi = std::clamp<int64_t>(hi, lo, len);
    if (asc) {
        if (hi - lo == 1) return vm.charStr((unsigned char)s[(size_t)lo]);
        return vm.vstr(s.substr((size_t)lo, (size_t)(hi - lo)));
    }
    size_t i = 0, cp = 0, bstart = s.size(), bend = s.size();
    for (; i <= s.size(); i++) {
        if (i == s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) {
            if ((int64_t)cp == lo && bstart == s.size()) bstart = i;
            if ((int64_t)cp == hi) {
                bend = i;
                break;
            }
            cp++;
        }
    }
    if (bstart > bend) bstart = bend;
    return vm.vstr(s.substr(bstart, bend - bstart));
}
NATIVE(s_pad) {
    // "x".pad(5) pads on the right, pad(-5) on the left
    std::string s = S(a[0]);
    int64_t w = vm.argInt(a, 1, "pad");
    std::string fill = n > 2 ? vm.argStr(a, 2, "pad")->s : " ";
    if (fill.empty()) fill = " ";
    int64_t len = (int64_t)(VM::ascii(as<ObjString>(a[0])) ? s.size() : utf8Length(s));
    int64_t target = w < 0 ? -w : w;
    std::string pad;
    for (int64_t i = len; i < target; i++) pad += fill;
    return vm.vstr(w < 0 ? pad + s : s + pad);
}

// ------------------------------------------------------------------ errors

NATIVE(e_kind) { return vm.vstr(as<ObjError>(a[0])->kind); }
NATIVE(e_msg) { return vm.vstr(as<ObjError>(a[0])->msg); }
NATIVE(e_cause) { return as<ObjError>(a[0])->payload; }
NATIVE(e_log) {
    flushOut();
    std::string prefix = n > 1 ? vm.toStr(a[1]) + " " : "";
    std::fprintf(stderr, "%s%s\n", prefix.c_str(), vm.toStr(a[0]).c_str());
    return Value();
}

static Value errorCtor(VM& vm, Value* a, int n) {
    std::string kind = as<ObjString>(vm.nativeData)->s;
    if (n == 0) return vm.newError(kind, kind);
    if (isObjType(a[0], OT::String)) return vm.newError(kind, as<ObjString>(a[0])->s, n > 1 ? a[1] : Value());
    std::string msg = kind;
    if (isObjType(a[0], OT::Error)) msg = as<ObjError>(a[0])->msg;
    return vm.newError(kind, msg, a[0]);
}

static Value errorFallback(VM& vm, ObjNamespace*, const std::string& name, bool forCall) {
    if (!forCall) return vm.newError(name, name);
    auto* nat = vm.alloc<ObjNative>("Error." + name, errorCtor, 0, 2);
    nat->data = vm.vstr(name);
    return Value::object(nat);
}

// ------------------------------------------------------------------ registration

void registerCore(VM& vm) {
    vm.defineNative("print", n_print, 0, -1, "fn(any...)", "Prints the values separated by spaces, then a newline.");
    vm.defineNative("write", n_write, 0, -1, "fn(any...)", "Prints the values without separators or newline.");
    vm.defineNative("input", n_input, 0, 1, "fn(str?) -> str?", "Reads a line from standard input (nil at end of input).");
    vm.defineNative("str", n_str, 1, 1, "fn(any) -> str", "Converts any value to its display string.");
    vm.defineNative("int", n_int, 1, 1, "fn(any) -> int ! Error", "Converts to an int (strings may fail with Error.InvalidNumber).");
    vm.defineNative("float", n_float, 1, 1, "fn(any) -> float ! Error", "Converts to a float.");
    vm.defineNative("bool", n_bool, 1, 1, "fn(any) -> bool", "true unless the value is nil, false or an error.");
    vm.defineNative("type", n_type, 1, 1, "fn(any) -> str", "The name of a value's type.");
    vm.defineNative("error", n_error, 0, 2, "fn(str, str?) -> Error", "Creates an error value: error(msg) or error(msg, kind).");
    vm.defineNative("is_error", n_is_error, 1, 1, "fn(any) -> bool", "True if the value is an error.");
    vm.defineNative("panic", n_panic, 0, 1, "fn(str?)", "Stops the program with a message and a stack trace.");
    vm.defineNative("assert", n_assert, 1, 2, "fn(bool, str?)", "Panics if the condition is false.");
    vm.defineNative("assert_eq", n_assert_eq, 2, 3, "fn(any, any, str?)", "Panics unless the two values are equal.");
    vm.defineNative("exit", n_exit, 0, 1, "fn(int?)", "Exits the program with a status code.");
    vm.defineNative("__main_args", n_main_args, 2, 2, "fn(list, int) -> any", "");
    vm.defineNative("range", n_range, 1, 3, "fn(int, int?, int?) -> range", "range(end), range(start, end) or range(start, end, step), end exclusive.");
    vm.defineNative("ord", n_ord, 1, 1, "fn(str) -> int", "Unicode code point of the first character.");
    vm.defineNative("chr", n_chr, 1, 1, "fn(int) -> str", "The character for a Unicode code point.");
    vm.defineNative("freeze", n_freeze, 1, 1, "fn(T) -> T",
                    "A deeply read-only copy of data (lists, maps, sets, tuples, strings, numbers). par tasks share "
                    "frozen values instead of copying them; they live until the program ends.");
    vm.defineNative("frozen", n_frozen, 1, 1, "fn(any) -> bool", "True if the value was made with freeze().");
    vm.defineNative("memory", n_memory, 0, 0, "fn() -> int", "Bytes of memory the program's objects currently use (approximately).");
    vm.defineNative("gc", n_gc, 0, 0, "fn() -> bool", "Runs the garbage collector now.");
    vm.defineNative("min", m_min, 1, -1, "fn(any...) -> any", "The smallest of the arguments.");
    vm.defineNative("max", m_max, 1, -1, "fn(any...) -> any", "The largest of the arguments.");
    vm.defineNative("abs", m_abs, 1, 1, "fn(num) -> num", "Absolute value.");
    vm.defineNative("sqrt", m_sqrt, 1, 1, "fn(num) -> float", "Square root.");

    auto* errNs = vm.defineNamespace("Error", "Error kinds: Error.NotFound, Error.MaxRetries(\"why\"), ...");
    errNs->getFallback = errorFallback;
    errNs->callable = vm.makeNative("Error", n_error, 0, 2);

    for (Tag t : {Tag::Int, Tag::Float}) {
        vm.defineMethod(t, "abs", m_abs, 0, 0, "fn() -> num", "Absolute value.");
        vm.defineMethod(t, "sqrt", m_sqrt, 0, 0, "fn() -> float", "Square root.");
        vm.defineMethod(t, "root", m_root, 1, 1, "fn(num) -> num", "The n-th root: 27.root(3) is 3 (an int when exact).");
        vm.defineMethod(t, "pow", m_pow, 1, 1, "fn(num) -> num", "Raises to a power.");
        vm.defineMethod(t, "sin", m_sin, 0, 0, "fn() -> float", "Sine (radians).");
        vm.defineMethod(t, "cos", m_cos, 0, 0, "fn() -> float", "Cosine (radians).");
        vm.defineMethod(t, "tan", m_tan, 0, 0, "fn() -> float", "Tangent (radians).");
        vm.defineMethod(t, "asin", m_asin, 0, 0, "fn() -> float", "Arc sine.");
        vm.defineMethod(t, "acos", m_acos, 0, 0, "fn() -> float", "Arc cosine.");
        vm.defineMethod(t, "atan", m_atan, 0, 1, "fn(num?) -> float", "Arc tangent; y.atan(x) is atan2.");
        vm.defineMethod(t, "ln", m_ln, 0, 0, "fn() -> float", "Natural logarithm.");
        vm.defineMethod(t, "log", m_log, 0, 1, "fn(num?) -> float", "Logarithm, base 10 by default.");
        vm.defineMethod(t, "exp", m_exp, 0, 0, "fn() -> float", "e raised to this power.");
        vm.defineMethod(t, "floor", m_floor, 0, 0, "fn() -> int", "Rounds down.");
        vm.defineMethod(t, "ceil", m_ceil, 0, 0, "fn() -> int", "Rounds up.");
        vm.defineMethod(t, "round", m_round, 0, 1, "fn(int?) -> num", "Rounds to the nearest int, or to n digits.");
        vm.defineMethod(t, "int", m_int, 0, 0, "fn() -> int", "Drops the fractional part.");
        vm.defineMethod(t, "float", m_float, 0, 0, "fn() -> float", "Converts to float.");
        vm.defineMethod(t, "clamp", m_clamp, 2, 2, "fn(num, num) -> num", "Limits the value to [min, max].");
        vm.defineMethod(t, "lerp", m_lerp, 2, 2, "fn(num, num) -> float", "a.lerp(b, t): linear interpolation.");
        vm.defineMethod(t, "min", m_min, 1, -1, "fn(num) -> num", "The smaller value.");
        vm.defineMethod(t, "max", m_max, 1, -1, "fn(num) -> num", "The larger value.");
        vm.defineMethod(t, "sign", m_sign, 0, 0, "fn() -> num", "-1, 0 or 1.");
        vm.defineMethod(t, "wait", m_wait, 0, 0, "fn()", "Pauses the current task for this many milliseconds.");
        vm.defineMethod(t, "is_nan", m_is_nan, 0, 0, "fn() -> bool", "True for NaN.");
    }
    vm.defineMethod(Tag::Int, "even", m_even, 0, 0, "fn() -> bool", "True for even numbers.");
    vm.defineMethod(Tag::Int, "odd", m_odd, 0, 0, "fn() -> bool", "True for odd numbers.");
    vm.defineMethod(Tag::Int, "hex", m_hex, 0, 0, "fn() -> str", "Hexadecimal digits.");
    vm.defineMethod(Tag::Int, "chr", n_chr, 0, 0, "fn() -> str", "The character with this code point.");

    Tag S_ = Tag::Str;
    vm.defineMethod(S_, "len", s_len, 0, 0, "fn() -> int", "Number of characters.");
    vm.defineMethod(S_, "up", s_up, 0, 0, "fn() -> str", "Upper-case copy.");
    vm.defineMethod(S_, "down", s_down, 0, 0, "fn() -> str", "Lower-case copy.");
    vm.defineMethod(S_, "trim", s_trim, 0, 0, "fn() -> str", "Removes surrounding whitespace.");
    vm.defineMethod(S_, "trim_start", s_trim_start, 0, 0, "fn() -> str", "Removes leading whitespace.");
    vm.defineMethod(S_, "trim_end", s_trim_end, 0, 0, "fn() -> str", "Removes trailing whitespace.");
    vm.defineMethod(S_, "cut", s_cut, 0, 1, "fn(str?) -> list[str]", "Splits on a separator (whitespace by default).");
    vm.defineMethod(S_, "split", s_cut, 0, 1, "fn(str?) -> list[str]", "Same as cut.");
    vm.defineMethod(S_, "join", s_join, 1, 1, "fn(list) -> str", "\", \".join(items): joins with this string between items.");
    vm.defineMethod(S_, "has", s_has, 1, 1, "fn(str) -> bool", "True if the substring occurs.");
    vm.defineMethod(S_, "starts", s_starts, 1, 1, "fn(str) -> bool", "True if it starts with the prefix.");
    vm.defineMethod(S_, "ends", s_ends, 1, 1, "fn(str) -> bool", "True if it ends with the suffix.");
    vm.defineMethod(S_, "find", s_find, 1, 1, "fn(str) -> int?", "Index of the substring, or nil.");
    vm.defineMethod(S_, "count", s_count, 1, 1, "fn(str) -> int", "Number of non-overlapping occurrences.");
    vm.defineMethod(S_, "replace", s_replace, 2, 2, "fn(str, str) -> str", "Replaces every occurrence.");
    vm.defineMethod(S_, "repeat", s_repeat, 1, 1, "fn(int) -> str", "The string repeated n times.");
    vm.defineMethod(S_, "rev", s_rev, 0, 0, "fn() -> str", "Reversed copy.");
    vm.defineMethod(S_, "lines", s_lines, 0, 0, "fn() -> list[str]", "Splits into lines.");
    vm.defineMethod(S_, "chars", s_chars, 0, 0, "fn() -> list[str]", "List of characters.");
    vm.defineMethod(S_, "bytes", s_bytes, 0, 0, "fn() -> list[int]", "UTF-8 bytes.");
    vm.defineMethod(S_, "num", s_num, 0, 0, "fn() -> num ! Error", "Parses an int or float.");
    vm.defineMethod(S_, "int", s_int, 0, 0, "fn() -> int ! Error", "Parses an int.");
    vm.defineMethod(S_, "float", s_float, 0, 0, "fn() -> float ! Error", "Parses a float.");
    vm.defineMethod(S_, "is_empty", s_is_empty, 0, 0, "fn() -> bool", "True for \"\".");
    vm.defineMethod(S_, "is_digit", s_is_digit, 0, 0, "fn() -> bool", "True if every character is 0-9.");
    vm.defineMethod(S_, "is_alpha", s_is_alpha, 0, 0, "fn() -> bool", "True if every character is a letter.");
    vm.defineMethod(S_, "is_space", s_is_space, 0, 0, "fn() -> bool", "True if every character is whitespace.");
    vm.defineMethod(S_, "slice", s_slice, 1, 2, "fn(int, int?) -> str", "Characters from start up to (not including) end.");
    vm.defineMethod(S_, "pad", s_pad, 1, 2, "fn(int, str?) -> str", "Pads to a width (negative width pads on the left).");

    vm.defineMethod(Tag::Error, "kind", e_kind, 0, 0, "fn() -> str", "The error kind, e.g. \"NotFound\".");
    vm.defineMethod(Tag::Error, "msg", e_msg, 0, 0, "fn() -> str", "The error message.");
    vm.defineMethod(Tag::Error, "cause", e_cause, 0, 0, "fn() -> any", "The value or error this error wraps.");
    vm.defineMethod(Tag::Error, "log_error", e_log, 0, 1, "fn(str?)", "Prints the error to stderr, with an optional prefix.");
}

}  // namespace ixo
