// I/O builtins: fs, os, time, json.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "builtins.hpp"
#include "json.hpp"
#include "simd.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ixo {

namespace fs = std::filesystem;

#define NATIVE(name) static Value name([[maybe_unused]] VM& vm, [[maybe_unused]] Value* a, [[maybe_unused]] int n)

static fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
static std::string U(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ------------------------------------------------------------------ processes

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string quoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (size_t i = 0;; i++) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            i++;
            backslashes++;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += arg[i];
        }
    }
    return out + "\"";
}
bool runProcess(const std::vector<std::string>& argv, const std::string& input, std::string& output, int& exitCode) {
    std::string cmd;
    for (size_t i = 0; i < argv.size(); i++) cmd += (i ? " " : "") + quoteArg(argv[i]);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR, outW, inR, inW;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return false;
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError = outW;
    si.hStdInput = inR;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = widen(cmd);
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outW);
    CloseHandle(inR);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(inW);
        return false;
    }
    DWORD written;
    if (!input.empty()) WriteFile(inW, input.data(), (DWORD)input.size(), &written, nullptr);
    CloseHandle(inW);
    char buf[4096];
    DWORD got;
    while (ReadFile(outR, buf, sizeof buf, &got, nullptr) && got > 0) output.append(buf, got);
    CloseHandle(outR);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    exitCode = (int)code;
    return true;
}
#else
bool runProcess(const std::vector<std::string>& argv, const std::string& input, std::string& output, int& exitCode) {
    int outPipe[2], inPipe[2];
    if (pipe(outPipe) != 0 || pipe(inPipe) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        dup2(inPipe[0], 0);
        dup2(outPipe[1], 1);
        dup2(outPipe[1], 2);
        close(outPipe[0]);
        close(inPipe[1]);
        std::vector<char*> args;
        for (auto& s : argv) args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(outPipe[1]);
    close(inPipe[0]);
    if (!input.empty()) {
        ssize_t w = write(inPipe[1], input.data(), input.size());
        (void)w;
    }
    close(inPipe[1]);
    char buf[4096];
    ssize_t got;
    while ((got = read(outPipe[0], buf, sizeof buf)) > 0) output.append(buf, (size_t)got);
    close(outPipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exitCode == 127 && output.empty()) return false;
    return true;
}
#endif

// ------------------------------------------------------------------ fs

static Value ioError(VM& vm, const std::string& path, const std::string& what) {
    std::error_code ec;
    if (!fs::exists(P(path), ec)) return vm.newError("NotFound", what + ": no such file or directory: " + path);
    return vm.newError("IO", what + " failed: " + path);
}

NATIVE(fs_read) {
    const std::string& path = vm.argStr(a, 0, "fs.read")->s;
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return ioError(vm, path, "read");
    std::ostringstream ss;
    ss << f.rdbuf();
    return vm.vstr(ss.str());
}
// Open files (fs.open): handles of the File class in the prelude
static std::mutex filesLock;
static std::unordered_map<int64_t, std::FILE*> openFiles;
static int64_t nextFile = 1;

NATIVE(fs_open) {
    const std::string& path = vm.argStr(a, 0, "fs.open")->s;
    std::string mode = n > 1 && !a[1].isNil() ? vm.argStr(a, 1, "fs.open")->s : "r";
    if (mode != "r" && mode != "w" && mode != "a")
        vm.panic("fs.open: the mode is \"r\" (read), \"w\" (write, replacing) or \"a\" (append), not \"" + mode + "\"");
#ifdef _WIN32
    std::FILE* fp = _wfopen(P(path).c_str(), (mode == "r" ? L"rb" : mode == "w" ? L"wb" : L"ab"));
#else
    std::FILE* fp = std::fopen(path.c_str(), (mode + "b").c_str());
#endif
    if (!fp) return ioError(vm, path, mode == "r" ? "open" : "open for writing");
    int ci = vm.builtinIndex("File");
    if (ci < 0 || !isObjType(vm.builtins[ci].value, OT::Class)) vm.panic("internal: the File class is missing");
    int64_t h;
    {
        std::lock_guard<std::mutex> g(filesLock);
        h = nextFile++;
        openFiles[h] = fp;
    }
    auto* cls = as<ObjClass>(vm.builtins[ci].value);
    Value inst = Value::object(vm.newInstance(cls));
    auto* o = as<ObjInstance>(inst);
    auto set = [&](const char* field, Value v) {
        auto it = cls->fieldIndex.find(vm.sym(field));
        if (it != cls->fieldIndex.end()) o->fields[(size_t)it->second] = v;
    };
    set("handle", Value::integer(h));
    set("path", vm.vstr(path));
    set("mode", vm.vstr(mode));
    return inst;
}
// fs.file_op(handle, op, text): 0 reads a line (nil at the end), 1 the rest, 2 writes, 3 closes
NATIVE(fs_file_op) {
    int64_t h = vm.argInt(a, 0, "fs.file_op"), op = vm.argInt(a, 1, "fs.file_op");
    std::FILE* fp = nullptr;
    {
        std::lock_guard<std::mutex> g(filesLock);
        auto it = openFiles.find(h);
        if (it != openFiles.end()) fp = it->second;
        if (op == 3 && fp) openFiles.erase(it);
    }
    if (op == 3) {
        if (fp) std::fclose(fp);
        return Value();
    }
    if (!fp) vm.panic("the file is closed");
    if (op == 0 || op == 1) {
        std::string s;
        char buf[4096];
        bool any = false;
        while (std::fgets(buf, sizeof buf, fp)) {
            any = true;
            s += buf;
            if (op == 0 && !s.empty() && s.back() == '\n') break;
        }
        if (op == 0) {
            if (!any) return Value();
            if (!s.empty() && s.back() == '\n') s.pop_back();
            if (!s.empty() && s.back() == '\r') s.pop_back();
        }
        return vm.vstr(s);
    }
    const std::string& text = vm.argStr(a, 2, "fs.file_op")->s;
    if (std::fwrite(text.data(), 1, text.size(), fp) != text.size()) return vm.newError("IO", "write failed");
    return Value();
}

NATIVE(fs_read_bytes) {
    const std::string& path = vm.argStr(a, 0, "fs.read_bytes")->s;
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return ioError(vm, path, "read");
    std::vector<Value> out;
    char c;
    while (f.get(c)) out.push_back(Value::integer((unsigned char)c));
    return vm.newList(std::move(out));
}
static Value writeFile(VM& vm, const std::string& path, const std::string& data, bool append) {
    std::ofstream f(P(path), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!f) return vm.newError("IO", "can't write to " + path);
    f << data;
    if (!f) return vm.newError("IO", "write failed: " + path);
    return Value();
}
NATIVE(fs_write) { return writeFile(vm, vm.argStr(a, 0, "fs.write")->s, vm.toStr(a[1]), false); }
NATIVE(fs_append) { return writeFile(vm, vm.argStr(a, 0, "fs.append")->s, vm.toStr(a[1]), true); }
NATIVE(fs_write_bytes) {
    std::string data;
    for (Value v : SeqView(*vm.argList(a, 1, "fs.write_bytes"))) {
        if (!v.isInt() || v.i < 0 || v.i > 255) vm.panic("fs.write_bytes() expects ints from 0 to 255");
        data += (char)v.i;
    }
    return writeFile(vm, vm.argStr(a, 0, "fs.write_bytes")->s, data, false);
}
NATIVE(fs_has) {
    std::error_code ec;
    return Value::boolean(fs::exists(P(vm.argStr(a, 0, "fs.has")->s), ec));
}
NATIVE(fs_is_dir) {
    std::error_code ec;
    return Value::boolean(fs::is_directory(P(vm.argStr(a, 0, "fs.is_dir")->s), ec));
}
NATIVE(fs_is_file) {
    std::error_code ec;
    return Value::boolean(fs::is_regular_file(P(vm.argStr(a, 0, "fs.is_file")->s), ec));
}
NATIVE(fs_drop) {
    const std::string& path = vm.argStr(a, 0, "fs.drop")->s;
    std::error_code ec;
    if (!fs::exists(P(path), ec)) return vm.newError("NotFound", "no such file or directory: " + path);
    if (!fs::remove(P(path), ec) || ec)
        return vm.newError("IO", "can't remove " + path + (fs::is_directory(P(path), ec) ? " (directory not empty)" : ""));
    return Value();
}
NATIVE(fs_make) {
    const std::string& path = vm.argStr(a, 0, "fs.make")->s;
    std::error_code ec;
    fs::create_directories(P(path), ec);
    if (ec) return vm.newError("IO", "can't create directory " + path + ": " + ec.message());
    return Value();
}
NATIVE(fs_list) {
    std::string path = n > 0 ? vm.argStr(a, 0, "fs.list")->s : ".";
    std::error_code ec;
    std::vector<std::string> names;
    for (auto& e : fs::directory_iterator(P(path), ec)) names.push_back(U(e.path().filename()));
    if (ec) return ioError(vm, path, "list");
    std::sort(names.begin(), names.end());
    std::vector<Value> out;
    for (auto& s : names) out.push_back(vm.vstr(s));
    return vm.newList(std::move(out));
}
NATIVE(fs_size) {
    const std::string& path = vm.argStr(a, 0, "fs.size")->s;
    std::error_code ec;
    auto sz = fs::file_size(P(path), ec);
    if (ec) return ioError(vm, path, "size");
    return Value::integer((int64_t)sz);
}
NATIVE(fs_cwd) {
    std::error_code ec;
    return vm.vstr(U(fs::current_path(ec)));
}
NATIVE(fs_join) {
    fs::path p;
    for (int i = 0; i < n; i++) p /= P(vm.argStr(a, i, "fs.join")->s);
    return vm.vstr(U(p));
}
NATIVE(fs_abs) {
    std::error_code ec;
    return vm.vstr(U(fs::absolute(P(vm.argStr(a, 0, "fs.abs")->s), ec)));
}

// ------------------------------------------------------------------ os

NATIVE(os_env) {
    const char* v = std::getenv(vm.argStr(a, 0, "os.env")->s.c_str());
    return v ? vm.vstr(v) : Value();
}
NATIVE(os_run) {
    std::vector<std::string> argv;
    if (isObjType(a[0], OT::String)) {
#ifdef _WIN32
        argv = {"cmd", "/c", as<ObjString>(a[0])->s};
#else
        argv = {"/bin/sh", "-c", as<ObjString>(a[0])->s};
#endif
    } else {
        for (Value v : SeqView(*vm.argList(a, 0, "os.run"))) argv.push_back(vm.toStr(v));
    }
    if (argv.empty()) vm.panic("os.run() needs a command");
    std::string input = n > 1 ? vm.toStr(a[1]) : "";
    std::string out;
    int code = 0;
    flushOut();
    if (!runProcess(argv, input, out, code)) return vm.newError("NotFound", "can't run '" + argv[0] + "'");
    return vm.newTuple({Value::integer(code), vm.vstr(out)});
}
NATIVE(os_cpu) {  // what the JIT and the list operations use
    const CpuInfo& c = cpuInfo();
    auto* m = vm.alloc<ObjMap>();
    Value keep = Value::object(m);
    m->set(vm.vstr("arch"), vm.vstr(c.arch));
    m->set(vm.vstr("simd"), vm.vstr(c.simd()));
    m->set(vm.vstr("cores"), Value::integer((int64_t)std::max(1u, std::thread::hardware_concurrency())));
    return keep;
}
NATIVE(os_platform) {
#ifdef _WIN32
    return vm.vstr("windows");
#elif defined(__APPLE__)
    return vm.vstr("macos");
#else
    return vm.vstr("linux");
#endif
}

// ------------------------------------------------------------------ time

static std::tm localParts(double epoch) {
    std::time_t t = (std::time_t)std::floor(epoch);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}
NATIVE(n_now) { return Value::object(vm.alloc<ObjTime>(nowSeconds())); }
NATIVE(n_clock) { return Value::number(monotonicSeconds() - vm.startTime); }
static double EP(Value v) { return as<ObjTime>(v)->epoch; }
NATIVE(tm_year) { return Value::integer(localParts(EP(a[0])).tm_year + 1900); }
NATIVE(tm_month) { return Value::integer(localParts(EP(a[0])).tm_mon + 1); }
NATIVE(tm_day) { return Value::integer(localParts(EP(a[0])).tm_mday); }
NATIVE(tm_hour) { return Value::integer(localParts(EP(a[0])).tm_hour); }
NATIVE(tm_min) { return Value::integer(localParts(EP(a[0])).tm_min); }
NATIVE(tm_sec) { return Value::integer(localParts(EP(a[0])).tm_sec); }
NATIVE(tm_ms) {
    double e = EP(a[0]);
    return Value::integer((int64_t)((e - std::floor(e)) * 1000));
}
NATIVE(tm_weekday) { return Value::integer(localParts(EP(a[0])).tm_wday); }
NATIVE(tm_unix) { return Value::number(EP(a[0])); }
NATIVE(tm_add) { return Value::object(vm.alloc<ObjTime>(EP(a[0]) + vm.argNum(a, 1, "add"))); }
NATIVE(tm_show) {
    std::string pat = n > 1 ? vm.argStr(a, 1, "show")->s : "YYYY-MM-DD HH:mm:ss";
    std::tm tm = localParts(EP(a[0]));
    auto two = [](int v) {
        char b[16];
        snprintf(b, sizeof b, "%02d", v);
        return std::string(b);
    };
    std::string out;
    for (size_t i = 0; i < pat.size();) {
        auto starts = [&](const char* t) { return pat.compare(i, std::char_traits<char>::length(t), t) == 0; };
        if (starts("YYYY")) { out += std::to_string(tm.tm_year + 1900); i += 4; }
        else if (starts("YY")) { out += two((tm.tm_year + 1900) % 100); i += 2; }
        else if (starts("MM")) { out += two(tm.tm_mon + 1); i += 2; }
        else if (starts("DD")) { out += two(tm.tm_mday); i += 2; }
        else if (starts("HH")) { out += two(tm.tm_hour); i += 2; }
        else if (starts("mm")) { out += two(tm.tm_min); i += 2; }
        else if (starts("SSS")) {
            char b[8];
            double e = EP(a[0]);
            snprintf(b, sizeof b, "%03d", (int)((e - std::floor(e)) * 1000));
            out += b;
            i += 3;
        }
        else if (starts("ss")) { out += two(tm.tm_sec); i += 2; }
        else out += pat[i++];
    }
    return vm.vstr(out);
}

// ------------------------------------------------------------------ json

// JSON straight between text and Ixo values (no tree in between).
namespace {

struct JsonReader {
    VM& vm;
    const std::string& t;
    size_t i = 0;
    std::string err;
    std::string buf;                                   // a string being decoded
    std::unordered_map<std::string, ObjString*> keys;  // map keys and short strings share one string
    std::vector<Value> items;                           // arrays being read (all levels), then exact lists
    std::vector<std::pair<Value, Value>> members;       // objects being read, then exact maps
    JsonReader(VM& v, const std::string& text) : vm(v), t(text) {}

    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t' || t[i] == '\v' ||
                                t[i] == '\f'))
            i++;
    }
    bool fail(const std::string& m) {
        if (err.empty()) err = m + " at offset " + std::to_string(i);
        return false;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    static int hex4(const std::string& t, size_t at) {
        int v = 0;
        for (size_t k = at; k < at + 4; k++) {
            char c = t[k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return v;  // like strtoul: stops at the first non-digit
            v = v * 16 + d;
        }
        return v;
    }
    // a string into buf
    bool str() {
        buf.clear();
        if (t[i] != '"') return fail("expected string");
        i++;
        size_t run = i;
        while (i < t.size() && t[i] != '"') {
            if (t[i] != '\\') {
                i++;
                continue;
            }
            buf.append(t, run, i - run);
            i++;
            if (i >= t.size()) return fail("bad escape");
            char e = t[i++];
            switch (e) {
                case '"': buf += '"'; break;
                case '\\': buf += '\\'; break;
                case '/': buf += '/'; break;
                case 'b': buf += '\b'; break;
                case 'f': buf += '\f'; break;
                case 'n': buf += '\n'; break;
                case 'r': buf += '\r'; break;
                case 't': buf += '\t'; break;
                case 'u': {
                    if (i + 4 > t.size()) return fail("bad \\u escape");
                    unsigned cp = (unsigned)hex4(t, i);
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        unsigned lo = (unsigned)hex4(t, i + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    utf8(buf, cp);
                    break;
                }
                default: return fail("bad escape");
            }
            run = i;
        }
        if (i >= t.size()) return fail("unterminated string");
        buf.append(t, run, i - run);
        i++;
        return true;
    }
    // the string in buf; repeated ones (keys, short values like "a" or "ok") share one string
    Value shared() {
        auto it = keys.find(buf);
        if (it != keys.end()) return Value::object(it->second);
        ObjString* s = vm.str(buf);
        if (keys.size() < 4096) keys.emplace(buf, s);
        return Value::object(s);
    }
    bool literal(const char* w, size_t n) {
        if (t.compare(i, n, w) != 0) return false;
        i += n;
        return true;
    }
    bool value(Value& out, int depth) {
        if (depth > 500) return fail("nesting too deep");
        ws();
        if (i >= t.size()) return fail("unexpected end");
        char c = t[i];
        if (c == '{') {
            i++;
            size_t first = members.size();
            ws();
            if (i < t.size() && t[i] == '}') {
                i++;
                out = Value::object(vm.alloc<ObjMap>());
                return true;
            }
            while (true) {
                ws();
                if (i >= t.size() || !str()) return fail("expected key");
                Value k = shared();
                ws();
                if (i >= t.size() || t[i] != ':') return fail("expected ':'");
                i++;
                Value v;
                if (!value(v, depth + 1)) return false;
                members.push_back({k, v});
                ws();
                if (i < t.size() && t[i] == ',') {
                    i++;
                    continue;
                }
                if (i < t.size() && t[i] == '}') {
                    i++;
                    auto* m = vm.alloc<ObjMap>();
                    m->entries.reserve(members.size() - first);
                    for (size_t k2 = first; k2 < members.size(); k2++) m->set(members[k2].first, members[k2].second);
                    members.resize(first);
                    out = Value::object(m);
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            i++;
            size_t first = items.size();
            ws();
            if (i < t.size() && t[i] == ']') {
                i++;
                out = vm.newList();
                return true;
            }
            while (true) {
                Value v;
                if (!value(v, depth + 1)) return false;
                items.push_back(v);
                ws();
                if (i < t.size() && t[i] == ',') {
                    i++;
                    continue;
                }
                if (i < t.size() && t[i] == ']') {
                    i++;
                    out = vm.newList(std::vector<Value>(items.begin() + (std::ptrdiff_t)first, items.end()));
                    items.resize(first);
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            if (!str()) return false;
            out = buf.size() <= 16 ? shared() : vm.vstr(buf);
            return true;
        }
        if (literal("true", 4)) {
            out = Value::boolean(true);
            return true;
        }
        if (literal("false", 5)) {
            out = Value::boolean(false);
            return true;
        }
        if (literal("null", 4)) {
            out = Value();
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* b = t.data() + i;
            const char* e = t.data() + t.size();
            // whole numbers stay exact ints; the rest are floats (whole floats too, below 9e15, as ints)
            int64_t iv;
            auto ri = std::from_chars(b, e, iv);
            double d;
            auto rd = std::from_chars(b, e, d);
            if (rd.ec != std::errc() && rd.ec != std::errc::result_out_of_range) return fail("bad number");
            if (ri.ec == std::errc() && ri.ptr == rd.ptr) {
                out = Value::integer(iv);
            } else if (d == std::floor(d) && std::fabs(d) < 9e15) {
                out = Value::integer((int64_t)d);
            } else {
                out = Value::number(d);
            }
            i += (size_t)(rd.ptr - b);
            return true;
        }
        return fail(std::string("unexpected character '") + c + "'");
    }
};

void jsonWrite(VM& vm, Value v, std::string& o, int indent, int level, int depth) {
    if (depth > 200) vm.panic("json.str(): value is nested too deeply (cycle?)");
    auto nl = [&](int lv) {
        if (indent < 0) return;
        o += '\n';
        o.append((size_t)(indent * lv), ' ');
    };
    switch (v.t) {
        case VT::Nil: o += "null"; return;
        case VT::Bool: o += v.b ? "true" : "false"; return;
        case VT::Int: {
            char b[24];
            auto r = std::to_chars(b, b + sizeof b, v.i);
            o.append(b, r.ptr);
            return;
        }
        case VT::Float: jsonNumber(o, v.f); return;
        default: break;
    }
    if (!v.isObj()) {
        o += "null";
        return;
    }
    auto array = [&](auto begin, auto end) {
        o += '[';
        bool first = true;
        for (auto it = begin; it != end; ++it) {
            if ((*it).isUndef()) continue;
            if (!first) o += ',';
            first = false;
            nl(level + 1);
            jsonWrite(vm, *it, o, indent, level + 1, depth + 1);
        }
        if (!first) nl(level);
        o += ']';
    };
    auto object = [&](const std::vector<std::pair<std::string, Value>>& members) {
        o += '{';
        for (size_t k = 0; k < members.size(); k++) {
            if (k) o += ',';
            nl(level + 1);
            jsonQuote(o, members[k].first);
            o += indent < 0 ? ":" : ": ";
            jsonWrite(vm, members[k].second, o, indent, level + 1, depth + 1);
        }
        if (!members.empty()) nl(level);
        o += '}';
    };
    switch (v.o->type) {
        case OT::String: jsonQuote(o, as<ObjString>(v)->s); return;
        case OT::List: { SeqView sv(*as<ObjList>(v)); array(sv.begin(), sv.end()); return; }
        case OT::Tuple: array(as<ObjTuple>(v)->items.begin(), as<ObjTuple>(v)->items.end()); return;
        case OT::Set: array(as<ObjSet>(v)->items.begin(), as<ObjSet>(v)->items.end()); return;
        case OT::Map: {
            auto* m = as<ObjMap>(v);
            bool allStrings = true;
            for (auto& [k, x] : m->entries)
                if (!k.isUndef() && !isObjType(k, OT::String)) allStrings = false;
            if (allStrings) {  // the usual case: names are already unique
                o += '{';
                bool first = true;
                for (auto& [k, x] : m->entries) {
                    if (k.isUndef()) continue;
                    if (!first) o += ',';
                    first = false;
                    nl(level + 1);
                    jsonQuote(o, as<ObjString>(k)->s);
                    o += indent < 0 ? ":" : ": ";
                    jsonWrite(vm, x, o, indent, level + 1, depth + 1);
                }
                if (!first) nl(level);
                o += '}';
                return;
            }
            // other keys become text, and two of them may read the same: the later value wins
            std::vector<std::pair<std::string, Value>> members;
            std::unordered_map<std::string, size_t> at;
            for (auto& [k, x] : m->entries) {
                if (k.isUndef()) continue;
                std::string name = vm.toStr(k);
                auto it = at.find(name);
                if (it != at.end()) {
                    members[it->second].second = x;
                } else {
                    at.emplace(name, members.size());
                    members.push_back({std::move(name), x});
                }
            }
            object(members);
            return;
        }
        case OT::Instance: {
            auto* inst = as<ObjInstance>(v);
            std::vector<std::pair<std::string, Value>> members;
            for (size_t k = 0; k < inst->fields.size(); k++) members.push_back({inst->cls->fields[k], inst->fields[k]});
            object(members);
            return;
        }
        default: jsonQuote(o, vm.toStr(v)); return;
    }
}

}  // namespace

std::string jsonEncode(VM& vm, Value v, int indent) {
    std::string o;
    jsonWrite(vm, v, o, indent, 0, 0);
    return o;
}

NATIVE(json_parse) {
    const std::string& text = vm.argStr(a, 0, "json.parse")->s;
    JsonReader r(vm, text);
    Value out;
    if (!r.value(out, 0)) return vm.newError("InvalidJson", r.err);
    r.ws();
    if (r.i != text.size()) return vm.newError("InvalidJson", "trailing characters at offset " + std::to_string(r.i));
    return out;
}
NATIVE(json_str) {
    int indent = n > 1 ? (int)vm.argInt(a, 1, "json.str") : -1;
    return vm.vstr(jsonEncode(vm, a[0], indent));
}

void registerIO(VM& vm) {
    auto* f = vm.defineNamespace("fs", "Files and directories.");
    vm.nativeModules.push_back("fs");
    vm.nsFunc(f, "read", fs_read, 1, 1, "fn(str) -> str ! Error", "Reads a whole text file.");
    vm.nsFunc(f, "open", fs_open, 1, 2, "fn(str, str?) -> File ! Error",
              "Opens a file to read (\"r\", the default) line by line, or to write (\"w\") or append (\"a\"); close it, or bind it with use.");
    vm.nsFunc(f, "file_op", fs_file_op, 2, 3, "fn(int, int, str?) -> any", "Used by File (fs.open).");
    vm.nsFunc(f, "read_bytes", fs_read_bytes, 1, 1, "fn(str) -> list[int] ! Error", "Reads a file as bytes.");
    vm.nsFunc(f, "write", fs_write, 2, 2, "fn(str, any) -> nil ! Error", "Writes (replaces) a file.");
    vm.nsFunc(f, "append", fs_append, 2, 2, "fn(str, any) -> nil ! Error", "Appends to a file.");
    vm.nsFunc(f, "write_bytes", fs_write_bytes, 2, 2, "fn(str, list[int]) -> nil ! Error", "Writes bytes to a file.");
    vm.nsFunc(f, "has", fs_has, 1, 1, "fn(str) -> bool", "True if the path exists.");
    vm.nsFunc(f, "is_dir", fs_is_dir, 1, 1, "fn(str) -> bool", "True for a directory.");
    vm.nsFunc(f, "is_file", fs_is_file, 1, 1, "fn(str) -> bool", "True for a regular file.");
    vm.nsFunc(f, "drop", fs_drop, 1, 1, "fn(str) -> nil ! Error", "Deletes a file or an empty directory.");
    vm.nsFunc(f, "make", fs_make, 1, 1, "fn(str) -> nil ! Error", "Creates a directory (and its parents).");
    vm.nsFunc(f, "list", fs_list, 0, 1, "fn(str?) -> list[str] ! Error", "Names in a directory.");
    vm.nsFunc(f, "size", fs_size, 1, 1, "fn(str) -> int ! Error", "File size in bytes.");
    vm.nsFunc(f, "cwd", fs_cwd, 0, 0, "fn() -> str", "The current directory.");
    vm.nsFunc(f, "join", fs_join, 1, -1, "fn(str...) -> str", "Joins path parts.");
    vm.nsFunc(f, "abs", fs_abs, 1, 1, "fn(str) -> str", "Absolute path.");

    auto* os = vm.defineNamespace("os", "The operating system: environment, processes.");
    vm.nativeModules.push_back("os");
    vm.nsFunc(os, "env", os_env, 1, 1, "fn(str) -> str?", "An environment variable, or nil.");
    vm.nsFunc(os, "run", os_run, 1, 2, "fn(any, str?) -> (int, str) ! Error",
              "Runs a command (a string for the shell, or a list of arguments); returns (exit_code, output).");
    vm.nsFunc(os, "platform", os_platform, 0, 0, "fn() -> str", "\"windows\", \"linux\" or \"macos\".");
    vm.nsFunc(os, "cpu", os_cpu, 0, 0, "fn() -> map[str, any]",
              "The processor: {arch, simd (the widest vector instructions used), cores}.");
    Value argsList = vm.newList();
    vm.nsValue(os, "args", argsList, "list[str]", "Command-line arguments after the script name.");
    vm.defineBuiltin("args", argsList, "list[str]", "Command-line arguments after the script name.");

    vm.defineNative("now", n_now, 0, 0, "fn() -> time", "The current date and time.");
    vm.defineNative("clock", n_clock, 0, 0, "fn() -> float", "Seconds since the program started (for timing).");
    Tag T_ = Tag::Time;
    vm.defineMethod(T_, "year", tm_year, 0, 0, "fn() -> int", "Year.");
    vm.defineMethod(T_, "month", tm_month, 0, 0, "fn() -> int", "Month 1-12.");
    vm.defineMethod(T_, "day", tm_day, 0, 0, "fn() -> int", "Day of the month.");
    vm.defineMethod(T_, "hour", tm_hour, 0, 0, "fn() -> int", "Hour 0-23.");
    vm.defineMethod(T_, "min", tm_min, 0, 0, "fn() -> int", "Minute.");
    vm.defineMethod(T_, "sec", tm_sec, 0, 0, "fn() -> int", "Second.");
    vm.defineMethod(T_, "ms", tm_ms, 0, 0, "fn() -> int", "Milliseconds.");
    vm.defineMethod(T_, "weekday", tm_weekday, 0, 0, "fn() -> int", "0 = Sunday.");
    vm.defineMethod(T_, "unix", tm_unix, 0, 0, "fn() -> float", "Seconds since 1970-01-01 UTC.");
    vm.defineMethod(T_, "add", tm_add, 1, 1, "fn(num) -> time", "The time n seconds later.");
    vm.defineMethod(T_, "show", tm_show, 0, 1, "fn(str?) -> str", "Formats with YYYY MM DD HH mm ss SSS.");


    auto* json = vm.defineNamespace("json", "JSON encoding and decoding.");
    vm.nativeModules.push_back("json");
    vm.nsFunc(json, "parse", json_parse, 1, 1, "fn(str) -> any ! Error", "Parses JSON text.");
    vm.nsFunc(json, "str", json_str, 1, 2, "fn(any, int?) -> str", "Encodes a value as JSON (optionally indented).");
}

}  // namespace ixo
