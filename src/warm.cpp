// Warm start (opt-in: IXO_JIT_CACHE=1): which functions got hot in earlier runs,
// so the next run compiles them after a few calls instead of a few hundred.
//
// A function is known by a hash of its name and bytecode. An edited function
// hashes differently and is simply not found, so a stale profile can only make
// a run compile later, never change what it does. Each entry also keeps the
// function's loop entry points, so a hot loop is entered in machine code early.
//
// The profile of a program lives in `.ixo-cache/` at its project root (the
// folder with ixo.toml), else in the user's cache folder, one file per main
// file; `ixo cache clean` empties it. An executable from `ixo build` reads a
// profile bundled as `.ixo-warm`.
//
// Off by default: today's compiler is fast enough that starting warm measured no
// gain (ROADMAP, Phase 15). It is here for Phase 12's slower optimizing tier.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "bundle.hpp"
#include "program.hpp"
#include "toml.hpp"
#include "vm.hpp"

#ifdef _WIN32
#include <process.h>
#define IXO_GETPID _getpid
#else
#include <unistd.h>
#define IXO_GETPID getpid
#endif

namespace ixo {

namespace fs = std::filesystem;

namespace {

fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string S(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

uint64_t fnv(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

const char* kMagic = "ixo-warm 1";
const size_t kMaxEntries = 8192;  // per program
const size_t kMaxFiles = 256;     // per cache folder: the oldest go first

}  // namespace

uint64_t warmKey(const ObjFunction* fn) {
    uint64_t h = fnv(fn->name.data(), fn->name.size());
    h = fnv(fn->chunk.code.data(), fn->chunk.code.size(), h);
    uint32_t arity = (uint32_t)fn->arity;
    return fnv(&arity, sizeof arity, h);
}

bool warmEnabled() {
    const char* e = std::getenv("IXO_JIT_CACHE");  // opt-in: measured no gain yet (see ROADMAP Phase 15)
    return e && std::string(e) == "1";
}

std::string warmCacheDir(const std::string& mainPath) {
    std::string root = findProjectRoot(S(P(mainPath).parent_path()));
    if (!root.empty()) return S(P(root) / ".ixo-cache");
#ifdef _WIN32
    if (const char* d = std::getenv("LOCALAPPDATA")) return S(P(d) / "ixo" / "cache");
#else
    if (const char* d = std::getenv("XDG_CACHE_HOME"); d && *d) return S(P(d) / "ixo");
    if (const char* h = std::getenv("HOME")) return S(P(h) / ".cache" / "ixo");
#endif
    return "";
}

static std::string profilePath(const std::string& mainPath) {
    std::string dir = warmCacheDir(mainPath);
    if (dir.empty()) return "";
    std::string canon = canonicalPath(mainPath);
    char name[40];
    std::snprintf(name, sizeof name, "%016llx.warm", (unsigned long long)fnv(canon.data(), canon.size()));
    return S(P(dir) / name);
}

static void parseProfile(const std::string& text, WarmProfile& w) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind(kMagic, 0) != 0) return;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key)) continue;
        uint64_t k = std::strtoull(key.c_str(), nullptr, 16);
        auto& seeds = w.fns[k];
        uint32_t at;
        int depth;
        char colon;
        while (ls >> at >> colon >> depth)
            if (colon == ':' && depth >= 0) seeds.push_back({at, depth});
    }
}

void VM::warmLoad(const std::string& mainPath) {
    // an executable from `ixo build --train` starts warm whatever IXO_JIT_CACHE says
    const Bundle* bundled = activeBundle();
    bool trained = bundled && vfsIsFile(S(P(bundled->root) / ".ixo-warm"));
    if (!jitOn || (!warmEnabled() && !trained)) return;
    auto w = std::make_shared<WarmProfile>();
    std::string text;
    if (const Bundle* b = activeBundle()) {
        // an executable from `ixo build --train`: its profile, read-only
        if (vfsRead(S(P(b->root) / ".ixo-warm"), text)) parseProfile(text, *w);
    } else {
        w->savePath = profilePath(mainPath);
        bool ok = false;
        if (!w->savePath.empty()) text = readFile(w->savePath, &ok);
        if (ok) parseProfile(text, *w);
    }
    if (const char* out = std::getenv("IXO_WARM_OUT")) w->savePath = out;  // ixo build --train
    warm = w;
}

// Not compiled at once: the first calls and loop turns fill the inline caches,
// which machine code relies on (compiled before them, it is generic and slower).
// A known function compiles after kWarmCalls more calls, its loops after
// kWarmTurns more turns, instead of the usual 200 and 2000.
static const uint32_t kWarmCalls = 8, kWarmTurns = 16;

void VM::warmApply(ObjFunction* fn) {
    fn->warmChecked = true;
    if (fn->noJit || fn->jitState != 0) return;
    auto it = warm->fns.find(warmKey(fn));
    if (it == warm->fns.end()) return;
    for (auto& s : it->second) {
        if (s.first >= fn->chunk.code.size()) continue;
        if (std::find(fn->jitSeeds.begin(), fn->jitSeeds.end(), s) == fn->jitSeeds.end()) fn->jitSeeds.push_back(s);
    }
    fn->hotness = std::max(fn->hotness, it->second.empty() ? kJitCalls - kWarmCalls : kJitTurns - kWarmTurns);
    warm->hits++;
}

void VM::warmSave() {
    if (!warm || warm->savePath.empty()) return;
    WarmProfile& w = *warm;
    bool changed = false;
    auto note = [&](Obj* o) {
        if (o->type != OT::Function) return;
        auto* fn = static_cast<ObjFunction*>(o);
        if (fn->jitState != 2) return;
        auto [it, isNew] = w.fns.try_emplace(warmKey(fn));
        changed |= isNew;
        for (auto& s : fn->jitSeeds)
            if (std::find(it->second.begin(), it->second.end(), s) == it->second.end()) {
                it->second.push_back(s);
                changed = true;
            }
    };
    pool.each(note);
    for (Obj* o : largeObjects) note(o);
    if (!changed) {
        // nothing new: leave the file alone, but mark it used (the size cap drops unused ones)
        std::error_code ec;
        if (fs::exists(P(w.savePath), ec)) fs::last_write_time(P(w.savePath), fs::file_time_type::clock::now(), ec);
        return;
    }
    std::string out = std::string(kMagic) + "\n";
    size_t n = 0;
    for (auto& [k, seeds] : w.fns) {
        if (++n > kMaxEntries) break;
        char key[24];
        std::snprintf(key, sizeof key, "%016llx", (unsigned long long)k);
        out += key;
        for (auto& s : seeds) out += " " + std::to_string(s.first) + ":" + std::to_string(s.second);
        out += "\n";
    }
    std::error_code ec;
    fs::path dst = P(w.savePath);
    if (dst.has_parent_path()) fs::create_directories(dst.parent_path(), ec);
    // write a private file, then rename it over the old one: concurrent runs never see half a profile
    fs::path tmp = dst;
    tmp += ".tmp" + std::to_string((long long)IXO_GETPID());
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return;
        o << out;
        if (!o) {
            o.close();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return;
    }
    if (std::getenv("IXO_WARM_OUT")) return;
    // keep the cache folder small: the least recently used profiles go first
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    for (auto& e : fs::directory_iterator(dst.parent_path(), ec))
        if (e.path().extension() == ".warm") files.push_back({e.last_write_time(ec), e.path()});
    if (files.size() > kMaxFiles) {
        std::sort(files.begin(), files.end());
        for (size_t i = 0; i + kMaxFiles < files.size(); i++) fs::remove(files[i].second, ec);
    }
}

// `ixo cache clean`: empties the cache of the project here (or of this file's
// project) and the user's cache folder.
int runCacheCommand(const std::vector<std::string>& args) {
    if (args.empty() || args[0] != "clean") {
        std::fprintf(stderr, "usage: ixo cache clean [file.ixo]\n");
        return 2;
    }
    std::string from = args.size() > 1 ? args[1] : S(fs::current_path() / "main.ixo");
    std::vector<std::string> dirs = {warmCacheDir(from)};
#ifdef _WIN32
    if (const char* d = std::getenv("LOCALAPPDATA")) dirs.push_back(S(P(d) / "ixo" / "cache"));
#else
    if (const char* d = std::getenv("XDG_CACHE_HOME"); d && *d) dirs.push_back(S(P(d) / "ixo"));
    else if (const char* h = std::getenv("HOME")) dirs.push_back(S(P(h) / ".cache" / "ixo"));
#endif
    size_t removed = 0;
    std::error_code ec;
    for (auto& d : dirs) {
        if (d.empty()) continue;
        for (auto& e : fs::directory_iterator(P(d), ec))
            if (e.path().extension() == ".warm" && fs::remove(e.path(), ec)) removed++;
    }
    std::printf("removed %zu cached profile%s\n", removed, removed == 1 ? "" : "s");
    return 0;
}

}  // namespace ixo
