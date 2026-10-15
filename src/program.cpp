#include "program.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "builtins.hpp"
#include "bundle.hpp"
#include "checker.hpp"
#include "parser.hpp"
#include "resolver.hpp"
#include "toml.hpp"

namespace ixo {

namespace fs = std::filesystem;

extern const char* const kPreludeSource;

std::string canonicalPath(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(std::u8string(path.begin(), path.end())), ec);
    if (ec) p = fs::absolute(fs::path(std::u8string(path.begin(), path.end())), ec);
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

Program::Program() {
    if (std::getenv("IXO_GC_STRESS")) vm.gcStress = true;
    vm.onPanic = [this](const std::string& msg, const std::vector<StackEntry>& trace) { printPanic(msg, trace); };
    if (const char* p = std::getenv("IXO_PATH")) {
        std::string s = p;
#ifdef _WIN32
        char sep = ';';
#else
        char sep = ':';
#endif
        size_t start = 0;
        while (start <= s.size()) {
            size_t e = s.find(sep, start);
            std::string part = s.substr(start, e == std::string::npos ? std::string::npos : e - start);
            if (!part.empty()) searchPaths.push_back(part);
            if (e == std::string::npos) break;
            start = e + 1;
        }
    }
    loadPrelude();
    if (const Bundle* b = activeBundle())  // a built program: its modules are inside the executable
        for (auto& [path, src] : b->files) overlays[path] = src;
}

Program::~Program() = default;

std::vector<std::string> Program::builtinNames() {
    std::vector<std::string> out;
    for (auto& b : vm.builtins)
        if (!b.name.empty() && b.name[0] != '$') out.push_back(b.name);
    return out;
}

std::string Program::sourceOf(const std::string& path) {
    auto it = sources.find(path);
    return it == sources.end() ? "" : it->second;
}

void Program::printDiagnostics(const Diagnostics& d) {
    flushOut();
    for (auto& diag : d.list) std::fputs(formatDiagnostic(diag, sourceOf(diag.file)).c_str(), stderr);
}

void Program::printPanic(const std::string& msg, const std::vector<StackEntry>& trace) {
    flushOut();
    std::string out = "panic: " + msg + "\n";
    for (auto& e : trace) {
        if (e.file.empty() && e.function.rfind("<module", 0) == 0) continue;
        out += "  at " + e.function + " (" + (e.file.empty() ? "?" : e.file) + ":" + std::to_string(e.line) + ")\n";
    }
    std::fputs(out.c_str(), stderr);
}

ModuleUnit* Program::nativeUnit(const std::string& name) {
    auto it = natives.find(name);
    if (it != natives.end()) return it->second;
    int idx = vm.builtinIndex(name);
    if (idx < 0 || !isObjType(vm.builtins[idx].value, OT::Namespace)) return nullptr;
    units.push_back(std::make_unique<ModuleUnit>());
    ModuleUnit* u = units.back().get();
    u->name = name;
    u->isNative = true;
    for (auto& [k, v] : as<ObjNamespace>(vm.builtins[idx].value)->members)
        if (!k.empty() && k[0] != '$') u->nativeExports.push_back(k);
    std::sort(u->nativeExports.begin(), u->nativeExports.end());
    natives[name] = u;
    return u;
}

ModuleUnit* Program::importModule(const std::string& dotted, const std::string& fromPath, Diagnostics& diags,
                                  SrcPos pos, const std::string& file) {
    if (std::find(vm.nativeModules.begin(), vm.nativeModules.end(), dotted) != vm.nativeModules.end())
        return nativeUnit(dotted);
    std::string rel = dotted;
    std::replace(rel.begin(), rel.end(), '.', '/');
    rel += ".ixo";
    // Where to look: next to the importing file, the project root (the nearest
    // directory with ixo.toml), installed packages (ixo_modules/), then IXO_PATH.
    std::vector<std::string> candidates;
    auto join = [](const std::string& dir, const std::string& r) {
        auto u = (fs::path(std::u8string(dir.begin(), dir.end())) / fs::path(std::u8string(r.begin(), r.end()))).u8string();
        return std::string(u.begin(), u.end());
    };
    std::string importerDir;
    if (!fromPath.empty()) {
        fs::path from(std::u8string(fromPath.begin(), fromPath.end()));
        auto u = from.parent_path().u8string();
        importerDir = std::string(u.begin(), u.end());
        candidates.push_back(join(importerDir, rel));
    }
    std::string root = findProjectRoot(importerDir.empty() ? "." : importerDir);
    if (!root.empty() && canonicalPath(root) != canonicalPath(importerDir.empty() ? "." : importerDir))
        candidates.push_back(join(root, rel));
    // Packages are installed flat: a package inside <project>/ixo_modules/ finds
    // its own dependencies next to it, in the project's ixo_modules/.
    std::vector<std::string> roots;
    for (std::string r = root; !r.empty() && roots.size() < 8;) {
        roots.push_back(r);
        fs::path rp(std::u8string(r.begin(), r.end()));
        if (rp.parent_path().filename() != "ixo_modules") break;
        auto pu = rp.parent_path().parent_path().u8string();
        r = findProjectRoot(std::string(pu.begin(), pu.end()));
    }
    for (auto& root : roots) {
        // a package: ixo_modules/<name>/ (its main file, or a module inside it)
        std::string first = dotted.substr(0, dotted.find('.'));
        std::string pkgDir = join(join(root, "ixo_modules"), first);
        if (vfsIsDir(pkgDir)) {
            if (dotted.find('.') == std::string::npos) {
                std::string mainFile = "lib.ixo";
                std::string text;
                bool ok = vfsRead(join(pkgDir, "ixo.toml"), text);
                TomlDoc doc;
                if (ok && parseToml(text, doc, nullptr)) mainFile = doc.str("package", "main", mainFile);
                candidates.push_back(join(pkgDir, mainFile));
                candidates.push_back(join(pkgDir, first + ".ixo"));
            } else {
                std::string inner = rel.substr(first.size() + 1);
                candidates.push_back(join(pkgDir, inner));
            }
        }
        candidates.push_back(join(join(root, "ixo_modules"), rel));
    }
    for (auto& sp : searchPaths) candidates.push_back(join(sp, rel));
    std::vector<std::string> tried;
    for (auto& cs0 : candidates) {
        fs::path candidate(std::u8string(cs0.begin(), cs0.end()));
        auto cu = candidate.u8string();
        std::string cs(cu.begin(), cu.end());
        tried.push_back(cs);
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec) && !overlays.count(canonicalPath(cs))) continue;
        std::string canon = canonicalPath(cs);
        auto it = byPath.find(canon);
        if (it != byPath.end()) {
            if (it->second->resolving) {
                diags.error(file, pos, "circular import: '" + dotted + "' is already being loaded");
                return nullptr;
            }
            return it->second;
        }
        CompileOptions o = opts;
        o.isEntry = false;
        ModuleUnit* m = loadFile(canon, diags, o);
        if (m) m->name = dotted;
        if (m && m->rt) m->rt->name = dotted;
        return m;
    }
    std::string msg = "can't find module '" + dotted + "'";
    if (!tried.empty()) msg += " (looked for " + tried[0] + ")";
    diags.error(file, pos, msg, (int)dotted.size() + 4);
    return nullptr;
}

// strict = true in the ixo.toml of the file's project makes all its files strict
static bool projectStrict(const std::string& file) {
    static std::unordered_map<std::string, bool> cache;
    namespace fsys = std::filesystem;
    std::error_code ec;
    fsys::path dir = fsys::absolute(fsys::path(file), ec).parent_path();
    for (int depth = 0; depth < 32 && !dir.empty(); depth++) {
        auto c = cache.find(dir.string());
        if (c != cache.end()) return c->second;
        fsys::path toml = dir / "ixo.toml";
        if (fsys::exists(toml, ec)) {
            std::string text;
            bool on = false;
            if (vfsRead(toml.string(), text)) {
                size_t at = 0;
                while (at < text.size()) {
                    size_t nl = text.find('\n', at);
                    std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
                    line.erase(std::remove_if(line.begin(), line.end(), [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }),
                               line.end());
                    if (line == "strict=true") on = true;
                    if (nl == std::string::npos) break;
                    at = nl + 1;
                }
            }
            cache[dir.string()] = on;
            return on;
        }
        if (dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return false;
}

bool Program::analyze(ModuleUnit* u, Diagnostics& diags, CompileOptions o) {
    Lexer lexer(u->source, u->path, diags);
    auto tokens = lexer.tokenize();
    ParseOptions po;
    po.prelude = u->isPrelude;
    Parser parser(std::move(tokens), u->path, diags, po);
    u->ast = parser.parseProgram();
    u->strict = parser.strict || (!u->isPrelude && !u->path.empty() && u->path[0] != '<' && projectStrict(u->path));
    u->inferParams = u->strict || o.inferParams;
    int before = diags.errorCount;
    u->resolving = true;
    Resolver resolver(*u, diags, *this);
    resolver.resolveModule(u->ast.get());
    u->resolving = false;
    if (o.typeCheck && (diags.errorCount == before || o.alwaysCheck)) {
        Checker checker(*this, *u, diags);
        checker.check(u->ast.get());
    }
    return !diags.hasErrors();
}

ModuleUnit* Program::loadSource(const std::string& name, const std::string& path, const std::string& source,
                                Diagnostics& diags, CompileOptions o) {
    units.push_back(std::make_unique<ModuleUnit>());
    ModuleUnit* u = units.back().get();
    u->name = name;
    u->path = path;
    u->source = source;
    sources[path] = source;
    if (!path.empty() && path[0] != '<') byPath[canonicalPath(path)] = u;
    Diagnostics local;
    local.quietDeprecations = diags.quietDeprecations;
    bool ok = analyze(u, local, o);
    for (auto& d : local.list) {
        if (d.severity == Diagnostic::Error) diags.errorCount++;
        diags.list.push_back(d);
    }
    if (!ok) {
        u->failed = true;
        return u;
    }
    if (!o.compile) return u;
    u->rt = vm.newModule(name, path);
    Compiler compiler(vm, *u, diags);
    ObjFunction* fn = compiler.compileModule(u->ast.get(), o.isEntry, false);
    if (diags.hasErrors()) {
        u->failed = true;
        return u;
    }
    u->rt->script = vm.alloc<ObjClosure>(fn);
    return u;
}

ModuleUnit* Program::loadFile(const std::string& path, Diagnostics& diags, CompileOptions o) {
    std::string canon = canonicalPath(path);
    auto ov = overlays.find(canon);
    bool ok = true;
    std::string src = ov != overlays.end() ? ov->second : readFile(path, &ok);
    if (!ok) return nullptr;
    std::string name = o.isEntry ? "<main>" : fs::path(std::u8string(path.begin(), path.end())).stem().string();
    if (o.isEntry) setScriptPath(vm, path);
    return loadSource(name, path, src, diags, o);
}

bool Program::run(ModuleUnit* m) {
    if (!m || !m->rt || !m->rt->script) return false;
    return vm.runModule(m->rt);
}

void Program::loadPrelude() {
    units.push_back(std::make_unique<ModuleUnit>());
    ModuleUnit* u = units.back().get();
    u->name = "<prelude>";
    u->path = "<prelude>";
    u->source = kPreludeSource;
    u->isPrelude = true;
    sources[u->path] = u->source;
    Diagnostics diags;
    CompileOptions o;
    o.typeCheck = false;
    o.isEntry = false;
    if (!analyze(u, diags, o)) {
        printDiagnostics(diags);
        std::fprintf(stderr, "internal error: the prelude failed to compile\n");
        return;
    }
    u->rt = vm.newModule("<prelude>", "<prelude>");
    Compiler compiler(vm, *u, diags);
    ObjFunction* fn = compiler.compileModule(u->ast.get(), false, false);
    if (diags.hasErrors()) {
        printDiagnostics(diags);
        return;
    }
    u->rt->script = vm.alloc<ObjClosure>(fn);
    if (!vm.runModule(u->rt)) return;
    prelude = u;
    registerPreludeMethods(u);
}

static std::string signatureOf(FnDecl* f, bool skipSelf) {
    std::string s = "fn(";
    bool first = true;
    for (size_t i = skipSelf ? 1 : 0; i < f->params.size(); i++) {
        if (!first) s += ", ";
        first = false;
        s += f->params[i].type ? f->params[i].type->str() : "any";
    }
    s += ")";
    if (f->ret) s += " -> " + f->ret->str();
    return s;
}

void Program::registerPreludeMethods(ModuleUnit* m) {
    static const std::unordered_map<std::string, std::vector<Tag>> typeTags = {
        {"iter", {Tag::List, Tag::Tuple, Tag::Range, Tag::Set, Tag::Str, Tag::Map, Tag::Flow}},
        {"list", {Tag::List}}, {"map", {Tag::Map}}, {"set", {Tag::Set}}, {"str", {Tag::Str}},
        {"int", {Tag::Int}}, {"float", {Tag::Float}}, {"tuple", {Tag::Tuple}}, {"range", {Tag::Range}},
    };
    // generic (iter) definitions first so type-specific ones replace them
    std::vector<Decl*> order(m->globalOrder.begin(), m->globalOrder.end());
    std::stable_sort(order.begin(), order.end(), [](Decl* a, Decl* b) {
        bool ai = a->name.rfind("iter__", 0) == 0, bi = b->name.rfind("iter__", 0) == 0;
        return ai && !bi;
    });
    for (Decl* d : order) {
        Value v = m->rt->globals[d->globalIndex];
        size_t sep = d->name.find("__");
        if (d->kind == DeclKind::Fn && sep != std::string::npos && d->node) {
            auto* f = static_cast<FnDecl*>(d->node);
            std::string type = d->name.substr(0, sep), method = d->name.substr(sep + 2);
            auto it = typeTags.find(type);
            if (it == typeTags.end()) continue;
            for (Tag t : it->second) {
                auto& table = vm.methods[(int)t];
                auto existing = table.find(vm.sym(method));
                if (existing != table.end() && isObjType(existing->second.fn, OT::Native)) continue;
                std::string doc;
                if (f->body && !f->body->stmts.empty() && f->body->stmts[0]->kind == NK::ExprStmt) {
                    Expr* e = static_cast<ExprStmt*>(f->body->stmts[0].get())->expr.get();
                    if (e->kind == NK::StrLit) doc = static_cast<StrLit*>(e)->value;
                }
                vm.defineMethodValue(t, method, v, signatureOf(f, true), doc);
            }
            continue;
        }
        if (d->kind == DeclKind::Fn) vm.defineBuiltin(d->name, v, d->node ? signatureOf(static_cast<FnDecl*>(d->node), false) : "", "");
        if (d->kind == DeclKind::Class) vm.defineBuiltin(d->name, v, "", "");
    }
}

}  // namespace ixo
