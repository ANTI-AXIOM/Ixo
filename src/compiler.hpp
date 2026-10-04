// Compiler: resolved AST -> bytecode. Tracks the exact stack depth so that
// block-scoped locals can live at any stack position (even in the middle of
// an expression, e.g. a match subject inside a call argument).
#pragma once

#include <functional>

#include "module.hpp"
#include "vm.hpp"

namespace ixo {

class Compiler {
public:
    Compiler(VM& vm, ModuleUnit& mod, Diagnostics& diags);
    // Compiles a module's top-level code. With `replValue`, the value of a
    // final expression statement is returned (for the REPL to print).
    ObjFunction* compileModule(Block* program, bool isEntry, bool replValue);

private:
    struct Local {
        Decl* decl;
        size_t slot;
    };
    struct Upval {
        bool isLocal;
        uint16_t index;
        Decl* decl;
    };
    struct LoopCtx {
        size_t continueTarget;  // code offset LOOP jumps to
        size_t depth;           // stack depth to unwind to for break/continue
        std::vector<size_t> breakJumps;
        size_t exitLevel = 0;   // exit actions registered before the loop
    };
    struct RetryCtx {
        size_t loopStart;
        size_t depth;
        size_t attemptSlot;
        size_t exitLevel = 0;
    };
    // Code that must run whenever a scope is left, by any path: defer blocks,
    // the end of memory regions, and waiting for `with tasks`.
    struct ExitAction {
        enum Kind { DeferBlock, RegionEnd, WithEnd } kind;
        Block* body;
        size_t scopeLevel;
    };
    struct FnState {
        FnState* enclosing = nullptr;
        ObjFunction* fn = nullptr;
        FuncInfo* info = nullptr;
        std::unordered_map<Decl*, size_t> slotOf;
        std::vector<Upval> upvals;
        size_t depth = 0;
        std::vector<size_t> scopeStarts;
        std::vector<LoopCtx> loops;
        std::vector<RetryCtx> retries;
        ClassInfo* cls = nullptr;
        ObjTypeDesc* retCheck = nullptr;
        std::vector<ExitAction> exits;
    };

    VM& vm;
    ModuleUnit& mod;
    Diagnostics& diags;
    FnState* fs = nullptr;
    int line = 1;

    void error(SrcPos p, const std::string& msg) { diags.error(mod.path, p, msg); }

    // emission
    Chunk& chunk() { return fs->fn->chunk; }
    void emitByte(uint8_t b);
    void emitOp(Op op, int effect);
    void emitU16(uint16_t v);
    void emitU32(uint32_t v);
    size_t emitJump(Op op, int effect);
    size_t compileCondJump(Expr* cond);  // jumps (to be patched) when cond is false
    uint16_t newIC();
    void emitIC();
    void patchJump(size_t operandAt);
    void emitLoop(size_t target);
    uint16_t constant(Value v);
    uint16_t strConst(const std::string& s) { return constant(vm.vstr(s)); }
    void emitConst(Value v);

    // scopes & variables
    void beginScope();
    void endScope(bool keepTop);
    void addLocal(Decl* d) { bindSlot(*fs, d, fs->depth - 1); }
    // Binds a variable to a stack slot and records it for debuggers.
    void bindSlot(FnState& s, Decl* d, size_t slot) {
        if (!d) return;
        s.slotOf[d] = slot;
        if (!d->name.empty() && d->name[0] != '$')
            s.fn->localNames.push_back({d->name, (uint16_t)slot, (uint32_t)s.fn->chunk.code.size(), UINT32_MAX});
    }
    void unwindTo(size_t depth);
    void emitExits(size_t fromIndex);
    void emitExit(const ExitAction& a);
    int resolveUpvalue(FnState* f, Decl* d);
    void emitGet(Decl* d, SrcPos p);
    void emitSet(Decl* d, SrcPos p);
    bool fallbackFor(Decl* d, uint8_t& kind, uint16_t& index);
    ObjTypeDesc* typeDesc(TypeExpr* t);
    void emitTypeCheck(TypeExpr* t, const std::string& what);

    // functions & classes
    ObjFunction* compileFunction(FnDecl* fn, const std::string& qualName);
    void emitClosure(ObjFunction* fn, FnState& st);
    void compileClass(ClassDecl* c);
    ObjFunction* compileCtor(ClassDecl* c);

    // statements & expressions
    void compileStmt(Stmt* s);
    void compileBlock(Block* b, bool wantValue);
    void compileExpr(Expr* e);
    void compileAssign(Assign* a);
    void compileIf(If* x, bool wantValue);
    void compileMatch(Match* m, bool wantValue);
    void compilePattern(Pattern* p, const std::function<void()>& load, std::vector<size_t>& fails);
    void compileCatch(Catch* c);
    void compileFor(For* f);
    void compileForRange(For* f, struct Range* r);
    void compileForSeq(For* f);
    std::unordered_set<Assign*> strAppends;  // `s += x` that grow a loop's own string
    void prepareStrBuilders(std::initializer_list<Node*> parts);
    bool growsGlobal(Decl* d);
    void compileForRecv(ForRecv* f);
    void compileMember(Member* m, bool raw);
    bool tryInlineHof(Member* m);
    bool m_selfField(Member* m);
    void compileFork(Fork* f);
    void compileRegion(Region* r, bool wantValue);
    void compileInterface(InterfaceDecl* d);
    bool hasNamed(const std::vector<ExprP>& args);
    void compileSelect(Select* s);
    void compileArgsNamed(const std::vector<ExprP>& args, SrcPos pos);
    void compileEnum(EnumDecl* e);
    void compileImports(Block* program);
};

}  // namespace ixo
