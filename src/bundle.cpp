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

int runBuild(const std::vector<std::string>& args) {
    std::string entry, out;
    bool typeCheck = true;
    for (size_t i = 0; i < args.size(); i++) {
        if ((args[i] == "-o" || args[i] == "--out") && i + 1 < args.size()) {
            out = args[++i];
        } else if (args[i] == "--no-typecheck") {
            typeCheck = false;
        } else if (args[i].rfind("-", 0) == 0) {
            std::fprintf(stderr, "ixo build: unknown option %s\n", args[i].c_str());
            return 2;
        } else if (entry.empty()) {
            entry = args[i];
        } else {
            std::fprintf(stderr, "usage: ixo build [file.ixo] [-o output]\n");
            return 2;
        }
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
    putU32(payload, (uint32_t)entryRel.size());
    payload += entryRel;

    std::string exe;
    if (!readWhole(selfExecutable(), exe)) {
        std::fprintf(stderr, "ixo build: can't read the ixo executable (%s)\n", selfExecutable().c_str());
        return 1;
    }
    exe.resize(exe.size() - payloadSize(exe));  // never two payloads
    if (out.empty()) {
        out = S(P(entry).stem());
#ifdef _WIN32
        out += ".exe";
#endif
    }
#ifdef _WIN32
    if (P(out).extension().empty()) out += ".exe";
#endif
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
    RunResult sign = runProcess({"codesign", "--force", "-s", "-", out}, "");
    if (!sign.started || sign.code != 0)
        std::fprintf(stderr, "ixo build: warning: couldn't re-sign %s (run: codesign --force -s - %s)\n", out.c_str(),
                     out.c_str());
#endif
    std::printf("built %s (%zu module%s, %zu KB of source)\n", out.c_str(), sources, sources == 1 ? "" : "s",
                (total + 1023) / 1024);
    return 0;
}

}  // namespace ixo
