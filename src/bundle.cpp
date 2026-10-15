#include "bundle.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#include "common.hpp"
#include "process.hpp"
#include "program.hpp"
#include "toml.hpp"

namespace ixo {

namespace {

namespace fs = std::filesystem;

const char kMagic[9] = "IXOBNDL1";

fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string S(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::unique_ptr<Bundle> gBundle;

void putU32(std::string& o, uint32_t v) {
    for (int i = 0; i < 4; i++) o += (char)((v >> (8 * i)) & 0xFF);
}
void putU64(std::string& o, uint64_t v) {
    for (int i = 0; i < 8; i++) o += (char)((v >> (8 * i)) & 0xFF);
}
uint64_t getLE(const std::string& s, size_t at, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v |= (uint64_t)(unsigned char)s[at + i] << (8 * i);
    return v;
}

bool readWhole(const std::string& path, std::string& out) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// The size of a bundle payload at the end of `exe` (including the trailer), or 0.
size_t payloadSize(const std::string& exe) {
    if (exe.size() < 16 || std::memcmp(exe.data() + exe.size() - 8, kMagic, 8) != 0) return 0;
    uint64_t n = getLE(exe, exe.size() - 16, 8);
    return n + 16 <= exe.size() ? (size_t)n + 16 : 0;
}

// The trailer alone, without reading the whole executable.
bool readPayload(const std::string& exePath, std::string& payload) {
    std::ifstream f(P(exePath), std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamoff size = f.tellg();
    if (size < 16) return false;
    char tail[16];
    f.seekg(size - 16);
    if (!f.read(tail, 16) || std::memcmp(tail + 8, kMagic, 8) != 0) return false;
    uint64_t n = getLE(std::string(tail, 16), 0, 8);
    if ((std::streamoff)n + 16 > size) return false;
    payload.resize((size_t)n);
    f.seekg(size - 16 - (std::streamoff)n);
    return (bool)f.read(payload.data(), (std::streamsize)n);
}

}  // namespace

const Bundle* activeBundle() { return gBundle.get(); }

bool loadBundleFromSelf() {
    std::string payload;
    std::string exe = selfExecutable();
    if (!readPayload(exe, payload)) return false;
    auto b = std::make_unique<Bundle>();
    fs::path exeDir = P(exe).parent_path();
    b->root = canonicalPath(S(exeDir / P("__" + S(P(exe).stem()) + "_bundle__")));
    size_t at = 0;
    auto need = [&](size_t n) { return at + n <= payload.size(); };
    if (!need(4)) return false;
    uint32_t count = (uint32_t)getLE(payload, at, 4);
    at += 4;
    for (uint32_t i = 0; i < count; i++) {
        if (!need(4)) return false;
        uint32_t nl = (uint32_t)getLE(payload, at, 4);
        at += 4;
        if (!need(nl + 8)) return false;
        std::string rel = payload.substr(at, nl);
        at += nl;
        uint64_t dl = getLE(payload, at, 8);
        at += 8;
        if (!need(dl)) return false;
        std::string key = canonicalPath(b->root + "/" + rel);
        b->files[key] = payload.substr(at, dl);
        at += dl;
        for (fs::path d = P(key).parent_path(); d.has_relative_path(); d = d.parent_path()) {
            if (!b->dirs.insert(S(d)).second) break;
            if (S(d) == b->root) break;
        }
    }
    if (!need(4)) return false;
    uint32_t el = (uint32_t)getLE(payload, at, 4);
    at += 4;
    if (!need(el)) return false;
    b->entry = canonicalPath(b->root + "/" + payload.substr(at, el));
    b->dirs.insert(b->root);
    gBundle = std::move(b);
    return true;
}

bool vfsIsFile(const std::string& path) {
    std::error_code ec;
    if (fs::is_regular_file(P(path), ec)) return true;
    return gBundle && gBundle->files.count(canonicalPath(path));
}

bool vfsIsDir(const std::string& path) {
    std::error_code ec;
    if (fs::is_directory(P(path), ec)) return true;
    return gBundle && gBundle->dirs.count(canonicalPath(path));
}

bool vfsRead(const std::string& path, std::string& out) {
    if (gBundle) {
        auto it = gBundle->files.find(canonicalPath(path));
        if (it != gBundle->files.end()) {
            out = it->second;
            return true;
        }
    }
    bool ok = false;
    out = readFile(path, &ok);
    return ok;
}

// ---------------------------------------------------------------- other machines
// `ixo build --target linux-arm64`: the bytecode is the program's sources, the same
// everywhere; what changes is the runtime it's appended to. The runtime for each
// target is kept in ~/.ixo/runtimes/<version>/ (IXO_HOME moves it), fetched once from
// the release (IXO_RUNTIME_URL moves that), or given with --runtime.
static const char* kTargets[] = {"windows-x64", "windows-arm64", "linux-x64", "linux-arm64", "macos-x64", "macos-arm64", "wasm"};

std::string hostTarget() {
#if defined(_WIN32)
    std::string os = "windows";
#elif defined(__APPLE__)
    std::string os = "macos";
#else
    std::string os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    return os + "-arm64";
#else
    return os + "-x64";
#endif
}

static std::string runtimeCache() {
    std::string home;
    if (const char* h = std::getenv("IXO_HOME")) home = h;
#ifdef _WIN32
    else if (const char* u = std::getenv("USERPROFILE")) home = S(P(u) / ".ixo");
#else
    else if (const char* u = std::getenv("HOME")) home = S(P(u) / ".ixo");
#endif
    return home.empty() ? "" : S(P(home) / "runtimes" / kIxoVersion);
}

static bool download(const std::string& url, const std::string& to, std::string& err) {
    std::error_code ec;
    fs::create_directories(P(to).parent_path(), ec);
    RunResult r = runProcess({"curl", "-fsSL", "--retry", "2", "-o", to + ".part", url}, "");
    if (!r.started || r.code != 0) {
        fs::remove(P(to + ".part"), ec);
        err = r.started ? "curl couldn't get " + url : "curl isn't here to fetch " + url;
        return false;
    }
    fs::rename(P(to + ".part"), P(to), ec);
    if (ec) {
        err = "can't write " + to;
        return false;
    }
    return true;
}

// The runtime for a target: the file (a folder for wasm), fetched if it isn't here yet.
static bool runtimeFor(const std::string& target, std::string& path, std::string& err) {
    std::string cache = runtimeCache();
    if (cache.empty()) {
        err = "no home folder for the runtime cache (set IXO_HOME), or give one with --runtime";
        return false;
    }
    std::string base = std::getenv("IXO_RUNTIME_URL") ? std::string(std::getenv("IXO_RUNTIME_URL"))
                                                        : "https://github.com/Eqixo/Ixo/releases/download/v" + std::string(kIxoVersion) + "/";
    if (!base.empty() && base.back() != '/') base += '/';
    std::error_code ec;
    if (target == "wasm") {
        path = S(P(cache) / "wasm");
        for (const char* f : {"ixo.js", "ixo.wasm", "gpu-worker.js", "coi-serviceworker.js"}) {
            std::string to = S(P(path) / f);
            if (fs::exists(P(to), ec)) continue;
            if (!download(base + "ixo-wasm/" + f, to, err)) return false;
        }
        return true;
    }
    std::string file = "ixo-" + target + (target.rfind("windows", 0) == 0 ? ".exe" : "");
    path = S(P(cache) / file);
    if (fs::exists(P(path), ec)) return true;
    return download(base + file, path, err);
}

// A web page that runs the program in the browser: the runtime's files, the
// program's files as a script, and a worker that starts ixo with them.
static int writeWebBuild(const std::string& runtimeDir, const std::string& outDir, const std::string& name,
                         const std::vector<std::pair<std::string, std::string>>& files, const std::string& entry) {
    std::error_code ec;
    fs::create_directories(P(outDir), ec);
    for (const char* f : {"ixo.js", "ixo.wasm", "gpu-worker.js", "coi-serviceworker.js"}) {
        fs::copy_file(P(runtimeDir) / f, P(outDir) / f, fs::copy_options::overwrite_existing, ec);
        if (ec && std::string(f) != "gpu-worker.js") {
            std::fprintf(stderr, "ixo build: the wasm runtime has no %s (in %s)\n", f, runtimeDir.c_str());
            return 1;
        }
    }
    auto js = [](const std::string& s) {
        std::string o = "\"";
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') o += '\\', o += (char)c;
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else o += (char)c;
        }
        return o + "\"";
    };
    std::string prog = "self.IXO_PROGRAM = {entry: " + js(entry) + ", files: {\n";
    for (auto& [rel, text] : files) prog += "  " + js(rel) + ": " + js(text) + ",\n";
    prog += "}};\n";
    std::string worker =
        "// Runs the program (ixo build --target wasm)\n"
        "importScripts(\"ixo.js\", \"program.js\");\n"
        "if (self.name !== \"em-pthread\") {\n"
        "  createIxo({\n"
        "    print: (s) => postMessage({ type: \"out\", text: s + \"\\n\" }),\n"
        "    printErr: (s) => postMessage({ type: \"err\", text: s + \"\\n\" }),\n"
        "    preRun: [(m) => {\n"
        "      for (const [p, text] of Object.entries(IXO_PROGRAM.files)) {\n"
        "        const full = \"/app/\" + p;\n"
        "        m.FS.mkdirTree(full.slice(0, full.lastIndexOf(\"/\")));\n"
        "        m.FS.writeFile(full, text);\n"
        "      }\n"
        "      m.FS.chdir(\"/app\");\n"
        "    }],\n"
        "    onExit: (code) => postMessage({ type: \"exit\", code }),\n"
        "  }).then((m) => m.callMain([\"/app/\" + IXO_PROGRAM.entry]));\n"
        "}\n";
    std::string page =
        "<!doctype html>\n<html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">\n"
        "<title>" + name + "</title>\n<script src=\"coi-serviceworker.js\"></script>\n"
        "<style>body{font:14px ui-monospace,monospace;margin:16px;background:#fff;color:#111}"
        "@media (prefers-color-scheme: dark){body{background:#111;color:#eee}}pre{white-space:pre-wrap}</style>\n"
        "</head><body><pre id=\"out\"></pre>\n<script>\n"
        "const out = document.getElementById(\"out\");\n"
        "if (window.crossOriginIsolated) {\n"
        "  const w = new Worker(\"run.js\");\n"
        "  w.onmessage = (e) => { const m = e.data; if (m.type === \"exit\") { if (m.code) out.textContent += \"[exit \" + m.code + \"]\\n\"; } else out.textContent += m.text; };\n"
        "}\n</script></body></html>\n";
    std::ofstream(P(outDir) / "program.js", std::ios::binary) << prog;
    std::ofstream(P(outDir) / "run.js", std::ios::binary) << worker;
    std::ofstream(P(outDir) / "index.html", std::ios::binary) << page;
    std::printf("built %s/ (serve it over http: python -m http.server -d %s)\n", outDir.c_str(), outDir.c_str());
    return 0;
}

int runBuild(const std::vector<std::string>& args) {
    std::string entry, out, target, runtime;
    bool typeCheck = true, train = false;
    std::vector<std::string> trainArgs;
    for (size_t i = 0; i < args.size(); i++) {
        if ((args[i] == "-o" || args[i] == "--out") && i + 1 < args.size()) {
            out = args[++i];
        } else if (args[i] == "--target" && i + 1 < args.size()) {
            target = args[++i];
        } else if (args[i] == "--runtime" && i + 1 < args.size()) {
            runtime = args[++i];
        } else if (args[i] == "--train") {
            train = true;
        } else if (args[i] == "--") {  // (with --train) the training run's arguments
            trainArgs.assign(args.begin() + (std::ptrdiff_t)i + 1, args.end());
            break;
        } else if (args[i] == "--no-typecheck") {
            typeCheck = false;
        } else if (args[i].rfind("-", 0) == 0) {
            std::fprintf(stderr, "ixo build: unknown option %s\n", args[i].c_str());
            return 2;
        } else if (entry.empty()) {
            entry = args[i];
        } else {
            std::fprintf(stderr, "usage: ixo build [file.ixo] [-o output] [--target name] [--runtime path] [--train [-- args]]\n");
            return 2;
        }
    }
    if (target.empty()) target = hostTarget();
    bool known = false;
    for (const char* t : kTargets) known |= target == t;
    if (!known) {
        std::string list;
        for (const char* t : kTargets) list += (list.empty() ? "" : ", ") + std::string(t);
        std::fprintf(stderr, "ixo build: unknown target '%s' (targets: %s)\n", target.c_str(), list.c_str());
        return 2;
    }
    std::error_code ec;
    if (entry.empty()) {
        // the project's main file
        std::string root = findProjectRoot(".");
        bool ok = false;
        TomlDoc doc;
        std::string text = root.empty() ? "" : readFile(S(P(root) / "ixo.toml"), &ok);
        if (ok && parseToml(text, doc, nullptr)) entry = S(P(root) / P(doc.str("package", "main", "main.ixo")));
        if (entry.empty() || !fs::is_regular_file(P(entry), ec)) {
            std::fprintf(stderr, "usage: ixo build [file.ixo] [-o output]   (no ixo.toml with a main file here)\n");
            return 2;
        }
    }
    // Load (and check) the program: what it imports is what goes in.
    Program prog;
    prog.opts.typeCheck = typeCheck;
    Diagnostics diags;
    ModuleUnit* m = prog.loadFile(entry, diags, prog.opts);
    if (!m) {
        std::fprintf(stderr, "ixo: can't read '%s'\n", entry.c_str());
        return 2;
    }
    prog.printDiagnostics(diags);
    if (diags.hasErrors()) {
        std::fprintf(stderr, "ixo build: fix the errors first\n");
        return 1;
    }
    std::vector<std::string> files;
    std::set<std::string> seen;
    auto add = [&](const std::string& f) {
        std::string c = canonicalPath(f);
        if (seen.insert(c).second) files.push_back(c);
    };
    std::string entryCanon = canonicalPath(entry);
    add(entryCanon);
    for (auto& u : prog.units)
        if (!u->isNative && !u->isPrelude && !u->path.empty() && u->path[0] != '<') add(u->path);
    // the ixo.toml files module lookup reads (the project's and each package's)
    size_t sources = files.size();
    for (size_t i = 0; i < sources; i++) {
        std::string r = findProjectRoot(S(P(files[i]).parent_path()));
        for (int depth = 0; !r.empty() && depth < 8; depth++) {
            add(S(P(r) / "ixo.toml"));
            fs::path rp = P(r);
            if (rp.parent_path().filename() != "ixo_modules") break;
            r = findProjectRoot(S(rp.parent_path().parent_path()));
        }
    }
    // everything relative to the folder that holds all of it
    fs::path base = P(files[0]).parent_path();
    for (auto& f : files) {
        fs::path dir = P(f).parent_path();
        fs::path common;
        auto a = base.begin(), b = dir.begin();
        for (; a != base.end() && b != dir.end() && *a == *b; ++a, ++b) common /= *a;
        if (common.empty()) {
            std::fprintf(stderr, "ixo build: '%s' and '%s' have no folder in common (different drives?)\n",
                         S(base).c_str(), f.c_str());
            return 1;
        }
        base = common;
    }
    std::string payload;
    putU32(payload, (uint32_t)files.size());
    size_t total = 0;
    for (auto& f : files) {
        std::string text;
        if (!readWhole(f, text)) {
            std::fprintf(stderr, "ixo build: can't read %s\n", f.c_str());
            return 1;
        }
        std::string rel = S(P(f).lexically_relative(base).generic_u8string());
        putU32(payload, (uint32_t)rel.size());
        payload += rel;
        putU64(payload, text.size());
        payload += text;
        total += text.size();
    }
    std::string entryRel = S(P(entryCanon).lexically_relative(base).generic_u8string());

    // --train: one run records which functions get hot; the executable starts warm with it
    std::string warmProfile;
    if (train) {
        std::string tmp = S(fs::temp_directory_path() / ("ixo-train-" + std::to_string(std::rand()) + ".warm"));
        std::vector<std::string> cmd = {selfExecutable(), "run", entry};
        cmd.insert(cmd.end(), trainArgs.begin(), trainArgs.end());
#ifdef _WIN32
        _putenv_s("IXO_WARM_OUT", tmp.c_str());
        _putenv_s("IXO_JIT_CACHE", "1");
#else
        setenv("IXO_WARM_OUT", tmp.c_str(), 1);
        setenv("IXO_JIT_CACHE", "1", 1);
#endif
        std::printf("training: ixo run %s ...\n", entry.c_str());
        std::fflush(stdout);
        RunResult r = runProcess(cmd, "");
#ifdef _WIN32
        _putenv_s("IXO_WARM_OUT", "");
#else
        unsetenv("IXO_WARM_OUT");
#endif
        if (!r.started || r.code != 0)
            std::fprintf(stderr, "ixo build: the training run %s (built without a profile)\n",
                         r.started ? ("exited with " + std::to_string(r.code)).c_str() : "didn't start");
        else if (!readWhole(tmp, warmProfile) || warmProfile.empty())
            std::fprintf(stderr, "ixo build: the training run left no profile (nothing got hot)\n");
        fs::remove(P(tmp), ec);
    }
    if (!warmProfile.empty()) {  // a file like the others, next to the program: .ixo-warm
        uint32_t count = 0;
        std::memcpy(&count, payload.data(), 4);
        count++;
        std::memcpy(payload.data(), &count, 4);
        putU32(payload, 9);
        payload += ".ixo-warm";
        putU64(payload, warmProfile.size());
        payload += warmProfile;
    }
    putU32(payload, (uint32_t)entryRel.size());
    payload += entryRel;

    if (target == "wasm") {  // a folder with a page that runs it
        std::string rdir = runtime;
        std::string why;
        if (rdir.empty() && !runtimeFor(target, rdir, why)) {
            std::fprintf(stderr, "ixo build: no wasm runtime (%s); build one with playground/build.sh and give its folder with --runtime\n",
                         why.c_str());
            return 1;
        }
        std::vector<std::pair<std::string, std::string>> texts;
        for (auto& f : files) {
            std::string text;
            readWhole(f, text);
            texts.push_back({S(P(f).lexically_relative(base).generic_u8string()), text});
        }
        if (out.empty()) out = S(P(entry).stem()) + "-web";
        return writeWebBuild(rdir, out, S(P(entry).stem()), texts, entryRel);
    }
    std::string exe, runtimePath = runtime;
    if (runtimePath.empty() && target == hostTarget()) runtimePath = selfExecutable();
    if (runtimePath.empty()) {
        std::string why;
        if (!runtimeFor(target, runtimePath, why)) {
            std::fprintf(stderr,
                         "ixo build: no ixo runtime for %s (%s): give one with --runtime path/to/ixo, or put it at %s\n",
                         target.c_str(), why.c_str(), S(P(runtimeCache()) / ("ixo-" + target)).c_str());
            return 1;
        }
    }
    if (!readWhole(runtimePath, exe)) {
        std::fprintf(stderr, "ixo build: can't read the ixo runtime (%s)\n", runtimePath.c_str());
        return 1;
    }
    exe.resize(exe.size() - payloadSize(exe));  // never two payloads
    bool winTarget = target.rfind("windows", 0) == 0;
    if (out.empty()) out = S(P(entry).stem()) + (winTarget ? ".exe" : "");
    else if (winTarget && P(out).extension().empty()) out += ".exe";
    if (fs::path parent = P(out).parent_path(); !parent.empty()) fs::create_directories(parent, ec);
    {
        std::ofstream o(P(out), std::ios::binary | std::ios::trunc);
        if (!o) {
            std::fprintf(stderr, "ixo build: can't write %s\n", out.c_str());
            return 1;
        }
        o.write(exe.data(), (std::streamsize)exe.size());
        o.write(payload.data(), (std::streamsize)payload.size());
        std::string trailer;
        putU64(trailer, payload.size());
        trailer.append(kMagic, 8);
        o.write(trailer.data(), (std::streamsize)trailer.size());
        if (!o) {
            std::fprintf(stderr, "ixo build: can't write %s\n", out.c_str());
            return 1;
        }
    }
#ifndef _WIN32
    fs::permissions(P(out), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
#endif
#ifdef __APPLE__
    // the appended files invalidate the signature; sign it again (ad hoc)
    RunResult sign = target.rfind("macos", 0) == 0 ? runProcess({"codesign", "--force", "-s", "-", out}, "") : RunResult{true, "", 0, "", ""};
    if (!sign.started || sign.code != 0)
        std::fprintf(stderr, "ixo build: warning: couldn't re-sign %s (run: codesign --force -s - %s)\n", out.c_str(),
                     out.c_str());
#endif
    std::printf("built %s for %s (%zu module%s, %zu KB of source%s)\n", out.c_str(), target.c_str(), sources,
                sources == 1 ? "" : "s", (total + 1023) / 1024, warmProfile.empty() ? "" : ", starts warm");
    return 0;
}

}  // namespace ixo
