// Resolver: binds every identifier to a Decl, decides which variables are
// globals / function locals / block locals / captured, builds class layouts,
// and turns placeholder arguments into lambdas (done by the parser).
//
// Scoping rules:
//  * `x = ...` at the top level of a file creates a module variable.
//  * `x = ...` inside a function creates a local of that function (hoisted to
//    the whole function), unless an enclosing *function* already has `x`, in
//    which case the closure variable is updated.
//  * Compound assignment (`+=`, `++`, ...) updates the nearest existing
//    variable, module variables included.
//  * Loop variables, `var` declarations, match bindings and `! err` names are
//    scoped to their block.
#pragma once

#include "module.hpp"

namespace ixo {

class Resolver {
public:
    Resolver(ModuleUnit& mod, Diagnostics& diags, ModuleHost& host);
    void resolveModule(Block* program);

private:
    struct Scope {
        std::unordered_map<std::string, Decl*> names;
        FuncInfo* fn;
    };
    ModuleUnit& mod;
    Diagnostics& diags;
    ModuleHost& host;
    std::vector<Scope> scopes;
    FuncInfo* curFn = nullptr;
    int loopDepth = 0;
    std::vector<Catch*> catches;
    ClassInfo* curClass = nullptr;
    InterfaceInfo* curIface = nullptr;  // resolving an interface's default methods
    EnumDecl* curEnum = nullptr;        // resolving an enum's shared methods
    int inDefer = 0;
    std::unordered_map<std::string, bool> warnedHides;

    void error(SrcPos p, const std::string& msg, int len = 1) { diags.error(mod.path, p, msg, len); }
    Decl* lookup(const std::string& name, bool markCapture = true);
    Decl* lookupLocalScopes(const std::string& name);
    Decl* declareLocal(const std::string& name, DeclKind k, SrcPos p, bool blockScoped);
    std::string suggest(const std::string& name);

    void prescanModule(Block* program);
    void processUse(Use* u);
    void collectAssigned(Node* n, std::vector<std::pair<std::string, SrcPos>>& out);

    void resolveStmt(Stmt* s);
    void resolveBlock(Block* b, bool newScope = true);
    void resolveExpr(Expr* e);
    void resolveTarget(Expr* target, bool plainAssign);
    void resolveFunction(FnDecl* fn, ClassInfo* cls);
    void resolveClass(ClassDecl* c);
    InterfaceInfo* interfaceInfo(InterfaceDecl* d);
    InterfaceInfo* findInterface(const std::string& name);
    void resolveEnum(EnumDecl* e);
    void resolvePattern(Pattern* p);
    void shortVariant(Pattern* p);
    // In a method: is `name` a field or method of the enclosing class (or a
    // method of the interface / enum)? Then a bare `name` means self.name.
    bool selfMemberName(const std::string& name);
    bool bindSelfMember(Ident* id, bool assigning);
};

}  // namespace ixo
