// A module being compiled: its source, AST and resolver/checker data.
#pragma once

#include <deque>
#include <unordered_map>

#include "ast.hpp"
#include "lexer.hpp"

namespace ixo {

struct ModuleRt;  // runtime side (vm.hpp)

struct ModuleUnit {
    std::string name;  // dotted name, "<main>", "<repl>" or "<prelude>"
    std::string path;  // file path ("" for native modules)
    std::string source;
    std::unique_ptr<Block> ast;
    std::vector<std::unique_ptr<Block>> replChunks;  // REPL inputs appended over time

    std::deque<Decl> decls;
    std::deque<FuncInfo> funcs;
    std::deque<ClassInfo> classes;
    std::deque<InterfaceInfo> interfaces;
    std::deque<EnumInfo> enums;
    FuncInfo* script = nullptr;  // the top-level code
    std::unordered_map<std::string, Decl*> globals;
    std::vector<Decl*> globalOrder;  // index == Decl::globalIndex
    std::unordered_map<std::string, Decl*> builtinDecls;  // cache of Builtin decls

    bool isNative = false;                 // math, fs, ... (namespaces implemented in C++)
    std::vector<std::string> nativeExports;
    bool isPrelude = false;
    bool resolving = false;  // import cycle detection
    bool failed = false;
    ModuleRt* rt = nullptr;
    std::vector<FnDecl*> tests;  // @test functions
    std::shared_ptr<void> typeArena;  // owns the checker's Type objects

    Decl* newDecl(const std::string& n, DeclKind k, SrcPos p) {
        decls.emplace_back();
        Decl* d = &decls.back();
        d->name = n;
        d->kind = k;
        d->pos = p;
        d->file = path;
        return d;
    }
    Decl* addGlobal(const std::string& n, DeclKind k, SrcPos p) {
        auto it = globals.find(n);
        if (it != globals.end()) return it->second;
        Decl* d = newDecl(n, k, p);
        d->isGlobal = true;
        d->module = this;
        d->globalIndex = (int)globalOrder.size();
        globalOrder.push_back(d);
        globals[n] = d;
        return d;
    }
    // Exported names: `*`-marked fns/classes (all globals for the prelude).
    Decl* findExport(const std::string& n) const {
        auto it = globals.find(n);
        if (it == globals.end()) return nullptr;
        Decl* d = it->second;
        if (isPrelude) return d;
        if (d->kind == DeclKind::Fn && d->node && static_cast<FnDecl*>(d->node)->exported) return d;
        if (d->kind == DeclKind::Class && d->node && static_cast<ClassDecl*>(d->node)->exported) return d;
        if (d->kind == DeclKind::Interface && d->node && static_cast<InterfaceDecl*>(d->node)->exported) return d;
        if (d->kind == DeclKind::Enum && d->node && static_cast<EnumDecl*>(d->node)->exported) return d;
        if (d->kind == DeclKind::Var && d->exported) return d;
        return nullptr;
    }
    std::vector<std::string> exportNames() const {
        if (isNative) return nativeExports;
        std::vector<std::string> out;
        for (Decl* d : globalOrder)
            if (findExport(d->name) == d) out.push_back(d->name);
        return out;
    }
};

// The function a name refers to (following imports), or null.
inline FnDecl* fnDeclOf(Decl* d) {
    for (int hops = 0; d && hops < 16; hops++) {
        if (d->kind == DeclKind::Fn) return d->node && d->node->kind == NK::FnDecl ? static_cast<FnDecl*>(d->node) : nullptr;
        if (d->kind != DeclKind::Import || !d->importFrom) return nullptr;
        auto it = d->importFrom->globals.find(d->importName);
        d = it == d->importFrom->globals.end() ? nullptr : it->second;
    }
    return nullptr;
}

// Is the function called whenever its name is used? Yes for one declared
// without a parameter list (fn tick, fn alive = hp > 0); a function written with
// parentheses is only called with them. Prelude methods (self first) follow the
// built-in rule: called when they need no arguments.
inline bool fnAutoCalls(const FnDecl* f) {
    if (!f || f->isLambda) return false;
    if (f->selfFirst) {
        for (size_t i = 1; i < f->params.size(); i++)
            if (!f->params[i].def) return false;
        return true;
    }
    return !f->hasParamList;
}

// Services the resolver needs from the driver.
class ModuleHost {
public:
    virtual ~ModuleHost() = default;
    // Finds a builtin by name; returns its index or -1.
    virtual int builtinIndex(const std::string& name) = 0;
    // Loads (lexes, parses, resolves, ...) the module `dotted` imported from `fromPath`.
    virtual ModuleUnit* importModule(const std::string& dotted, const std::string& fromPath,
                                     Diagnostics& diags, SrcPos pos, const std::string& file) = 0;
    // All builtin names (for "did you mean" suggestions and completion).
    virtual std::vector<std::string> builtinNames() = 0;
};

}  // namespace ixo
