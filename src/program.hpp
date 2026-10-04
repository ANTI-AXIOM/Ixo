// Program: the compilation driver. Loads modules (files, native modules and
// the prelude), runs every pass, and owns the VM.
#pragma once

#include <deque>
#include <memory>

#include "compiler.hpp"
#include "module.hpp"
#include "vm.hpp"

namespace ixo {

struct CompileOptions {
    bool typeCheck = true;
    bool compile = true;   // false: analysis only (check, LSP)
    bool isEntry = true;
    bool alwaysCheck = false;  // type-check even if names failed to resolve (editor support)
};

class Program : public ModuleHost {
public:
    Program();
    ~Program() override;

    VM vm;
    std::vector<std::string> searchPaths;
    std::deque<std::unique_ptr<ModuleUnit>> units;
    std::unordered_map<std::string, ModuleUnit*> byPath;
    std::unordered_map<std::string, ModuleUnit*> natives;
    std::unordered_map<std::string, std::string> sources;  // path -> text, for error display
    std::unordered_map<std::string, std::string> overlays; // unsaved editor buffers (LSP)
    CompileOptions opts;
    ModuleUnit* prelude = nullptr;

    // ModuleHost
    int builtinIndex(const std::string& name) override { return vm.builtinIndex(name); }
    ModuleUnit* importModule(const std::string& dotted, const std::string& fromPath, Diagnostics& diags, SrcPos pos,
                             const std::string& file) override;
    std::vector<std::string> builtinNames() override;

    // Loads, checks and compiles a module from source. Returns null only if
    // the file can't be read; check diags for errors.
    ModuleUnit* loadFile(const std::string& path, Diagnostics& diags, CompileOptions o);
    ModuleUnit* loadSource(const std::string& name, const std::string& path, const std::string& source,
                           Diagnostics& diags, CompileOptions o);
    // Runs a compiled module's top-level code. False if it panicked.
    bool run(ModuleUnit* m);

    void printDiagnostics(const Diagnostics& d);
    void printPanic(const std::string& msg, const std::vector<StackEntry>& trace);
    std::string sourceOf(const std::string& path);

    ModuleUnit* nativeUnit(const std::string& name);

private:
    void loadPrelude();
    void registerPreludeMethods(ModuleUnit* m);
    bool analyze(ModuleUnit* u, Diagnostics& diags, CompileOptions o);
};

std::string canonicalPath(const std::string& path);

}  // namespace ixo
