// ixo command-line interface.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "builtins.hpp"
#include "common.hpp"
#include "bundle.hpp"
#include "debugger.hpp"
#include "disasm.hpp"
#include "parser.hpp"
#include "profiler.hpp"
#include "program.hpp"
#include "tools.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

using namespace ixo;

static const char* kVersion = "0.6.0";

static void usage() {
    std::printf(
        "Ixo %s\n\n"
        "usage:\n"
        "  ixo                         start the interactive REPL\n"
        "  ixo <file.ixo> [args...]    run a program\n"
        "  ixo run <file.ixo> [args]   run a program\n"
        "  ixo check <files...>        report syntax and type errors without running\n"
        "  ixo fmt [--check] <files>   format files in place (--check: only report)\n"
        "  ixo test [files or dirs]    run @test functions (default: tests/)\n"
        "           [--filter text] [--json] [--watch]\n"
        "  ixo new <name>              create a project\n"
        "  ixo build [file] [-o out]   pack a program and its modules into one executable\n"
        "  ixo cache clean             forget hot functions remembered by IXO_JIT_CACHE=1 runs\n"
        "  ixo init [name]             add an ixo.toml to the current folder\n"
        "  ixo add <git-url|folder>    add a package (and install it)\n"
        "          [name] [--rev r]\n"
        "  ixo install                 install the packages in ixo.toml (as locked in ixo.lock)\n"
        "  ixo update [names]          move packages to their newest commits\n"
        "  ixo remove <names>          remove packages\n"
        "  ixo lsp                     start the language server (for editors)\n"
        "  ixo dap                     start the debug adapter (for editors)\n"
        "  ixo disasm <file.ixo>       show the compiled bytecode\n"
        "  ixo version                 print the version\n\n"
        "options:\n"
        "  --no-typecheck              skip the static type checker\n"
        "  --watch                     rerun when the program or its modules change\n"
        "  --profile                   run with the sampling profiler; prints a report to stderr\n"
        "  --memprofile                report allocations, collections and live memory to stderr\n"
        "  --profile-out=<file>        also write folded stacks (for flame graph tools)\n",
        kVersion);
}

struct RunOptions {
    bool typeCheck = true;
    bool watch = false;
    bool memprofile = false;
    bool profile = false;
    std::string profileOut;
};

// IXO_OPSTATS=1: counts the instructions a program runs (and common pairs),
// to see where the interpreter spends its time.
struct OpStats : Debugger {
    uint64_t counts[256] = {};
    std::map<std::pair<int, int>, uint64_t> pairs;
    int prev = -1;
    void check(VM&, Fiber* f) override {
        if (f->frames.empty()) return;
        int op = *f->frames.back().ip;
        counts[op]++;
        if (prev >= 0) pairs[{prev, op}]++;
        prev = op;
    }
    void onPanic(VM&, Fiber*, const std::string&) override {}
    void report() {
        uint64_t total = 0;
        for (auto c : counts) total += c;
        if (!total) return;
        std::vector<std::pair<uint64_t, int>> v;
        for (int i = 0; i < 256; i++)
            if (counts[i]) v.push_back({counts[i], i});
        std::sort(v.rbegin(), v.rend());
        std::fprintf(stderr, "\n-- %llu instructions --\n", (unsigned long long)total);
        for (size_t i = 0; i < v.size() && i < 20; i++)
            std::fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * (double)v[i].first / (double)total, opName((uint8_t)v[i].second));
        std::vector<std::pair<uint64_t, std::pair<int, int>>> pv;
        for (auto& [k, c] : pairs) pv.push_back({c, k});
        std::sort(pv.rbegin(), pv.rend());
        std::fprintf(stderr, "  pairs:\n");
        for (size_t i = 0; i < pv.size() && i < 15; i++)
            std::fprintf(stderr, "  %5.1f%%  %s + %s\n", 100.0 * (double)pv[i].first / (double)total,
                         opName((uint8_t)pv[i].second.first), opName((uint8_t)pv[i].second.second));
    }
};

static int cmdRun(const std::string& path, const std::vector<std::string>& args, const RunOptions& ro) {
    Program prog;
    prog.opts.typeCheck = ro.typeCheck;
    Value argList = prog.vm.builtins[prog.vm.builtinIndex("args")].value;
    for (auto& a : args) as<ObjList>(argList)->push(prog.vm.vstr(a));
    Diagnostics diags;
    ModuleUnit* m = prog.loadFile(path, diags, prog.opts);
    if (!m) {
        std::fprintf(stderr, "ixo: can't read '%s'\n", path.c_str());
        return 2;
    }
    prog.printDiagnostics(diags);
    if (diags.hasErrors()) return 1;
    Profiler profiler;
    ProfileHook profileHook;
    MemProfiler memprof;
    if (ro.profile) {
        prog.vm.profiler = &profiler;
        prog.vm.debugger = &profileHook;
        profiler.start(true);
    }
    if (ro.memprofile) prog.vm.memProfiler = &memprof;
    OpStats opstats;
    bool wantOpStats = std::getenv("IXO_OPSTATS") != nullptr;
    if (wantOpStats) prog.vm.debugger = &opstats;
    if (!ro.profile && !ro.memprofile && !wantOpStats) prog.vm.warmLoad(path);
    bool ok = prog.run(m);
    flushOut();
    prog.vm.warmSave();
    prog.vm.printJitStats();
    if (wantOpStats) {
        prog.vm.debugger = nullptr;
        opstats.report();
    }
    if (ro.memprofile) {
        prog.vm.memProfiler = nullptr;
        std::string rep = memprof.report(prog.vm);
        std::fwrite(rep.data(), 1, rep.size(), stderr);
    }
    if (ro.profile) {
        profiler.stop();
        prog.vm.profiler = nullptr;
        if (prog.vm.debugger == &profileHook) prog.vm.debugger = nullptr;
        if (ParRuntime* rt = ParRuntime::get()) {
            if (ok) {
                rt->shutdown();
                for (Profiler* wp : rt->workerProfilers()) profiler.merge(*wp);
            }
        }
        std::string rep = profiler.report();
        std::fwrite(rep.data(), 1, rep.size(), stderr);
        if (!ro.profileOut.empty()) {
            if (FILE* fo = std::fopen(ro.profileOut.c_str(), "wb")) {
                std::string folded = profiler.folded();
                std::fwrite(folded.data(), 1, folded.size(), fo);
                std::fclose(fo);
                std::fprintf(stderr, "  folded stacks written to %s\n", ro.profileOut.c_str());
            } else {
                std::fprintf(stderr, "  can't write %s\n", ro.profileOut.c_str());
            }
        }
    }
    if (ParRuntime* rt = ParRuntime::get(); rt && (ro.profile || ro.memprofile) && rt->tasksStarted > 0) {
        auto human = [](double b) {
            char s[32];
            if (b >= 1 << 20) std::snprintf(s, sizeof s, "%.1f MB", b / (1 << 20));
            else if (b >= 1024) std::snprintf(s, sizeof s, "%.1f KB", b / 1024);
            else std::snprintf(s, sizeof s, "%.0f bytes", b);
            return std::string(s);
        };
        std::fprintf(stderr, "\n-- par: %llu task%s; copied %s into them and %s back --\n",
                     (unsigned long long)rt->tasksStarted.load(), rt->tasksStarted == 1 ? "" : "s",
                     human((double)rt->bytesIn).c_str(), human((double)rt->bytesOut).c_str());
        if (rt->bytesIn > (1u << 20))
            std::fprintf(stderr, "   (large read-only data: freeze(x) shares it with tasks instead of copying it)\n");
    }
    if (ParRuntime* rt = ParRuntime::get()) {
        if (!ok) {
            // par workers may be in the middle of tasks: don't wait for them
            std::fflush(stderr);
            hardExit(1);
        }
        rt->shutdown();
    }
    return ok ? prog.vm.exitCode : 1;
}

static int cmdCheck(const std::vector<std::string>& files) {
    int errors = 0, warnings = 0;
    for (auto& f : files) {
        Program prog;
        prog.opts.compile = false;
        Diagnostics diags;
        ModuleUnit* m = prog.loadFile(f, diags, prog.opts);
        if (!m) {
            std::fprintf(stderr, "ixo: can't read '%s'\n", f.c_str());
            errors++;
            continue;
        }
        prog.printDiagnostics(diags);
        for (auto& d : diags.list) (d.severity == Diagnostic::Error ? errors : warnings)++;
    }
    if (errors == 0) std::fprintf(stderr, "ok: no errors%s\n", warnings ? (" (" + std::to_string(warnings) + " warnings)").c_str() : "");
    return errors ? 1 : 0;
}

// Prints the standard library reference as Markdown (docs/stdlib.md is generated with this).
static int cmdStdlibDocs() {
    Program prog;
    VM& vm = prog.vm;
    std::string out = "# Ixo standard library reference\n\n"
                      "_Generated by `ixo stdlib-docs` from the definitions in the interpreter._\n\n";
    out += "## Global functions\n\n| name | signature | description |\n|---|---|---|\n";
    std::vector<std::string> nss;
    for (auto& b : vm.builtins) {
        if (b.name.empty() || b.name[0] == '$') continue;
        if (isObjType(b.value, OT::Namespace)) {
            nss.push_back(b.name);
            continue;
        }
        out += "| `" + b.name + "` | `" + b.sig + "` | " + b.doc + " |\n";
    }
    for (auto& ns : nss) {
        int idx = vm.builtinIndex(ns);
        auto* o = as<ObjNamespace>(vm.builtins[idx].value);
        out += "\n## `" + ns + "`\n\n" + vm.builtins[idx].doc + "\n\n";
        std::vector<std::string> names;
        for (auto& [k, v] : o->members)
            if (!k.empty() && k[0] != '$') names.push_back(k);
        std::sort(names.begin(), names.end());
        if (names.empty()) continue;
        out += "| member | signature | description |\n|---|---|---|\n";
        for (auto& n : names) {
            auto it = vm.memberDocs.find(ns + "." + n);
            std::string sig = it != vm.memberDocs.end() ? it->second.first : "";
            std::string doc = it != vm.memberDocs.end() ? it->second.second : "";
            out += "| `" + ns + "." + n + "` | `" + sig + "` | " + doc + " |\n";
        }
    }
    const std::pair<Tag, const char*> tags[] = {
        {Tag::Int, "int"},       {Tag::Float, "float"}, {Tag::Str, "str"},   {Tag::List, "list"},
        {Tag::Tuple, "tuple"},   {Tag::Map, "map"},     {Tag::Set, "set"},   {Tag::Range, "range"},
        {Tag::Error, "Error"},   {Tag::Flow, "flow"},   {Tag::Task, "task"}, {Tag::Vec, "vec"},
        {Tag::Mat, "mat"},       {Tag::Quat, "quat"},   {Tag::Rng, "rng"},   {Tag::Time, "time"},
    };
    out += "\n## Methods by type\n\nMethods are called with a dot: `\"hi\".up`, `list.add(3)`. A method that takes no\n"
           "arguments can be called without parentheses.\n";
    for (auto& [tag, name] : tags) {
        auto& table = vm.methods[(int)tag];
        std::vector<std::string> names;
        for (auto& [s, mi] : table) names.push_back(vm.symName(s));
        std::sort(names.begin(), names.end());
        out += std::string("\n### ") + name + "\n\n| method | signature | description |\n|---|---|---|\n";
        for (auto& n : names) {
            auto& mi = table[vm.sym(n)];
            std::string doc = mi.doc;
            out += "| `." + n + "` | `" + mi.sig + "` | " + doc + " |\n";
        }
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

static int cmdDisasm(const std::string& path) {
    Program prog;
    Diagnostics diags;
    ModuleUnit* m = prog.loadFile(path, diags, prog.opts);
    if (!m) return 2;
    prog.printDiagnostics(diags);
    if (diags.hasErrors() || !m->rt) return 1;
    std::fputs(disassemble(prog.vm, m->rt->script->fn).c_str(), stdout);
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::vector<std::string> args(argv + 1, argv + argc);
    RunOptions ro;
    if (loadBundleFromSelf()) return cmdRun(activeBundle()->entry, args, ro);  // made by `ixo build`
    // options come before the file: ixo [options] [run] [options] file args...
    auto takeOptions = [&](size_t from) {
        while (from < args.size()) {
            const std::string& a = args[from];
            if (a == "--no-typecheck") {
                ro.typeCheck = false;
            } else if (a == "--watch") {
                ro.watch = true;
            } else if (a == "--memprofile") {
                ro.memprofile = true;
            } else if (a == "--profile") {
                ro.profile = true;
            } else if (a.rfind("--profile-out=", 0) == 0) {
                ro.profile = true;
                ro.profileOut = a.substr(14);
            } else {
                break;
            }
            args.erase(args.begin() + (long)from);
        }
    };
    takeOptions(0);
    if (!args.empty() && args[0] == "run") takeOptions(1);
    if (args.empty()) return runRepl();
    std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        usage();
        return 0;
    }
    if (cmd == "version" || cmd == "--version") {
        std::printf("ixo %s\n", kVersion);
        return 0;
    }
    if (cmd == "run") {
        if (rest.empty()) {
            usage();
            return 2;
        }
        if (ro.watch) {
            std::vector<std::string> child{"run"};
            if (!ro.typeCheck) child.push_back("--no-typecheck");
            child.insert(child.end(), rest.begin(), rest.end());
            return runWatch(child, {rest[0]});
        }
        return cmdRun(rest[0], std::vector<std::string>(rest.begin() + 1, rest.end()), ro);
    }
    if (cmd == "check") {
        if (rest.empty()) {
            usage();
            return 2;
        }
        return cmdCheck(rest);
    }
    if (cmd == "fmt" || cmd == "stdlib-docs") {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);  // keep LF line endings
#endif
    }
    if (cmd == "fmt") return runFormatter(rest);
    if (cmd == "test") {
        auto w = std::find(rest.begin(), rest.end(), "--watch");
        if (w != rest.end() || ro.watch) {
            if (w != rest.end()) rest.erase(w);
            std::vector<std::string> child{"test"};
            child.insert(child.end(), rest.begin(), rest.end());
            std::vector<std::string> targets;
            for (size_t i = 0; i < rest.size(); i++) {
                if (rest[i] == "--filter") {
                    i++;
                    continue;
                }
                if (rest[i].rfind("--", 0) != 0) targets.push_back(rest[i]);
            }
            if (targets.empty()) targets.push_back(std::filesystem::exists("tests") ? "tests" : ".");
            return runWatch(child, targets);
        }
        return runTests(rest);
    }
    if (cmd == "new") return runNewProject(rest);
    if (cmd == "build") return runBuild(rest);
    if (cmd == "cache") return runCacheCommand(rest);
    if (cmd == "init") return runInit(rest);
    if (cmd == "add") return runAdd(rest);
    if (cmd == "install") return runInstall(rest);
    if (cmd == "update") return runUpdate(rest);
    if (cmd == "remove") return runRemove(rest);
    if (cmd == "lsp") return runLanguageServer();
    if (cmd == "dap") return runDebugAdapter();
    if (cmd == "repl") return runRepl();
    if (cmd == "stdlib-docs") return cmdStdlibDocs();
    if (cmd == "disasm") {
        if (rest.empty()) {
            usage();
            return 2;
        }
        return cmdDisasm(rest[0]);
    }
    return cmdRun(cmd, rest, ro);
}
