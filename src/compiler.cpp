#include "compiler.hpp"
#include "gpu.hpp"

#include "checker.hpp"
#include "jit.hpp"

#include <algorithm>
#include <cctype>

namespace ixo {

// A name alone as a statement (`tick`, `cleanup`): a variable holding a function
// with no parameters is called. Declared functions follow their declaration
// (compiled as calls already, or a value that the checker warns about).
static bool autoCallStatement(Expr* e) {
    if (e->kind != NK::Ident) return false;
    auto* id = static_cast<Ident*>(e);
    return id->statementCall && !id->ref && !fnDeclOf(id->decl);
}

Compiler::Compiler(VM& vm, ModuleUnit& mod, Diagnostics& diags) : vm(vm), mod(mod), diags(diags) {}

// ------------------------------------------------------------------ emission

void Compiler::emitByte(uint8_t b) {
    chunk().code.push_back(b);
    chunk().lines.push_back(line);
}

void Compiler::emitOp(Op op, int effect) {
    emitByte(op);
    fs->depth = (size_t)((long long)fs->depth + effect);
}

void Compiler::emitU16(uint16_t v) {
    emitByte(v & 0xFF);
    emitByte(v >> 8);
}

void Compiler::emitU32(uint32_t v) {
    for (int i = 0; i < 4; i++) emitByte((v >> (8 * i)) & 0xFF);
}

uint16_t Compiler::newIC() {
    fs->fn->ic.emplace_back();
    if (fs->fn->ic.size() > 65535) error(SrcPos{line, 1}, "this function is too big (too many member accesses)");
    return (uint16_t)(fs->fn->ic.size() - 1);
}

void Compiler::emitIC() { emitU16(newIC()); }

// `if a < b` / `while i <= n`: compare and branch in one instruction.
size_t Compiler::compileCondJump(Expr* cond) {
    if (cond && cond->kind == NK::Binary) {
        auto* b = static_cast<Binary*>(cond);
        int cmp = -1;
        switch (b->op) {
            case BinOp::Lt: cmp = 0; break;
            case BinOp::Le: cmp = 1; break;
            case BinOp::Gt: cmp = 2; break;
            case BinOp::Ge: cmp = 3; break;
            case BinOp::Eq: cmp = 4; break;
            case BinOp::Ne: cmp = 5; break;
            default: break;
        }
        if (cmp >= 0) {
            compileExpr(b->left.get());
            compileExpr(b->right.get());
            line = b->pos.line;
            emitOp(OP_JUMP_IF_NOT_CMP, -2);
            emitByte((uint8_t)cmp);
            size_t at = chunk().code.size();
            emitU32(0);
            return at;
        }
    }
    compileExpr(cond);
    return emitJump(OP_JUMP_IF_FALSE, -1);
}

size_t Compiler::emitJump(Op op, int effect) {
    emitOp(op, effect);
    size_t at = chunk().code.size();
    emitU32(0);
    return at;
}

void Compiler::patchJump(size_t at) {
    size_t target = chunk().code.size();
    uint32_t off = (uint32_t)(target - (at + 4));
    for (int i = 0; i < 4; i++) chunk().code[at + i] = (off >> (8 * i)) & 0xFF;
}

void Compiler::emitLoop(size_t target) {
    emitOp(OP_LOOP, 0);
    uint32_t off = (uint32_t)(chunk().code.size() + 4 - target);
    emitU32(off);
}

uint16_t Compiler::constant(Value v) {
    auto& c = chunk().consts;
    // reuse identical ints / strings
    for (size_t i = 0; i < c.size(); i++) {
        const Value& x = c[i];
        if (x.t != v.t) continue;
        if (v.t == VT::Int && x.i == v.i) return (uint16_t)i;
        if (v.t == VT::Obj && x.o->type == OT::String && v.o->type == OT::String &&
            static_cast<ObjString*>(x.o)->s == static_cast<ObjString*>(v.o)->s)
            return (uint16_t)i;
    }
    if (c.size() >= 65535) {
        error({line, 1}, "too many constants in one function");
        return 0;
    }
    c.push_back(v);
    return (uint16_t)(c.size() - 1);
}

void Compiler::emitConst(Value v) {
    emitOp(OP_CONST, +1);
    emitU16(constant(v));
}

// ------------------------------------------------------------------ scopes

void Compiler::beginScope() { fs->scopeStarts.push_back(fs->depth); }

void Compiler::emitExit(const ExitAction& a) {
    switch (a.kind) {
        case ExitAction::DeferBlock: compileBlock(a.body, false); break;
        case ExitAction::RegionEnd: emitOp(OP_REGION_END, 0); break;
        case ExitAction::WithEnd: emitOp(OP_WITH_END, 0); break;
    }
}

// Emits the exit actions registered at index >= fromIndex, innermost first,
// for a jump out of their scopes (return, break, continue, retry, `?`).
void Compiler::emitExits(size_t fromIndex) {
    std::vector<ExitAction> copy(fs->exits.begin() + std::min(fromIndex, fs->exits.size()), fs->exits.end());
    for (size_t i = copy.size(); i-- > 0;) emitExit(copy[i]);
}

void Compiler::endScope(bool keepTop) {
    // actions registered in this scope run as it ends normally
    size_t level = fs->scopeStarts.size();
    while (!fs->exits.empty() && fs->exits.back().scopeLevel >= level) {
        ExitAction a = fs->exits.back();
        fs->exits.pop_back();
        emitExit(a);
    }
    size_t start = fs->scopeStarts.back();
    fs->scopeStarts.pop_back();
    size_t n = fs->depth - start - (keepTop ? 1 : 0);
    if (n > 0) {
        emitOp(keepTop ? OP_ENDSCOPE_KEEP : OP_ENDSCOPE, -(int)n);
        emitU16((uint16_t)n);
    }
    for (auto it = fs->slotOf.begin(); it != fs->slotOf.end();) {
        if (it->second >= start && it->first->blockScoped) {
            for (auto& ln : fs->fn->localNames)
                if (ln.slot == it->second && ln.end == UINT32_MAX && ln.name == it->first->name)
                    ln.end = (uint32_t)chunk().code.size();
            it = fs->slotOf.erase(it);
        } else {
            ++it;
        }
    }
}

void Compiler::unwindTo(size_t depth) {
    if (fs->depth > depth) {
        size_t saved = fs->depth;
        emitOp(OP_ENDSCOPE, 0);
        emitU16((uint16_t)(saved - depth));
    }
}

int Compiler::resolveUpvalue(FnState* f, Decl* d) {
    if (!f->enclosing) return -1;
    for (size_t i = 0; i < f->upvals.size(); i++)
        if (f->upvals[i].decl == d) return (int)i;
    auto it = f->enclosing->slotOf.find(d);
    if (it != f->enclosing->slotOf.end()) {
        f->upvals.push_back({true, (uint16_t)it->second, d});
        return (int)f->upvals.size() - 1;
    }
    int idx = resolveUpvalue(f->enclosing, d);
    if (idx < 0) return -1;
    f->upvals.push_back({false, (uint16_t)idx, d});
    return (int)f->upvals.size() - 1;
}

void Compiler::emitGet(Decl* d, SrcPos p) {
    if (!d) {
        emitOp(OP_NIL, +1);
        return;
    }
    if (d->kind == DeclKind::Builtin) {
        emitOp(OP_GET_BUILTIN, +1);
        emitU16((uint16_t)d->builtinIndex);
        return;
    }
    if (d->isGlobal) {
        emitOp(OP_GET_GLOBAL, +1);
        emitU16((uint16_t)d->globalIndex);
        fs->fn->globalsUsed.push_back((uint16_t)d->globalIndex);
        return;
    }
    auto it = fs->slotOf.find(d);
    if (it != fs->slotOf.end()) {
        bool mayBeUnset = d->mayBeUnset || (d->kind == DeclKind::Var && !d->blockScoped);
        emitOp(mayBeUnset ? OP_GET_LOCAL_CHK : OP_GET_LOCAL, +1);
        emitU16((uint16_t)it->second);
        if (mayBeUnset) emitU16(strConst(d->name));
        return;
    }
    int up = resolveUpvalue(fs, d);
    if (up < 0) {
        error(p, "internal: can't find variable '" + d->name + "'");
        emitOp(OP_NIL, +1);
        return;
    }
    emitOp(OP_GET_UPVAL, +1);
    emitU16((uint16_t)up);
    emitU16(strConst(d->name));
}

void Compiler::emitSet(Decl* d, SrcPos p) {
    if (!d) {
        emitOp(OP_POP, -1);
        return;
    }
    if (d->isGlobal && d->kind != DeclKind::Builtin) {
        emitOp(OP_SET_GLOBAL, -1);
        emitU16((uint16_t)d->globalIndex);
        fs->fn->globalsUsed.push_back((uint16_t)d->globalIndex);
        return;
    }
    auto it = fs->slotOf.find(d);
    if (it != fs->slotOf.end()) {
        emitOp(OP_SET_LOCAL, -1);
        emitU16((uint16_t)it->second);
        return;
    }
    int up = resolveUpvalue(fs, d);
    if (up < 0) {
        error(p, "internal: can't assign variable '" + d->name + "'");
        emitOp(OP_POP, -1);
        return;
    }
    emitOp(OP_SET_UPVAL, -1);
    emitU16((uint16_t)up);
}

bool Compiler::fallbackFor(Decl* d, uint8_t& kind, uint16_t& index) {
    kind = FB_NONE;
    index = 0;
    if (!d) return false;
    if (d->kind == DeclKind::Builtin) {
        kind = FB_BUILTIN;
        index = (uint16_t)d->builtinIndex;
        return true;
    }
    if (d->isGlobal) {
        kind = FB_GLOBAL;
        index = (uint16_t)d->globalIndex;
        fs->fn->globalsUsed.push_back(index);
        return true;
    }
    auto it = fs->slotOf.find(d);
    if (it != fs->slotOf.end()) {
        kind = FB_LOCAL;
        index = (uint16_t)it->second;
        return true;
    }
    int up = resolveUpvalue(fs, d);
    if (up >= 0) {
        kind = FB_UPVAL;
        index = (uint16_t)up;
        return true;
    }
    return false;
}

static bool isTypeVar(const std::string& n) { return n.size() == 1 && std::isupper((unsigned char)n[0]); }

ObjTypeDesc* Compiler::typeDesc(TypeExpr* t) {
    auto* d = vm.alloc<ObjTypeDesc>();
    d->text = t->str();
    switch (t->k) {
        case TypeExpr::Optional:
            d->k = ObjTypeDesc::Optional;
            d->inner = typeDesc(t->args[0].get());
            return d;
        case TypeExpr::Result:
            d->k = ObjTypeDesc::Result;
            d->inner = typeDesc(t->args[0].get());
            return d;
        case TypeExpr::Ref: {
            ObjTypeDesc* in = typeDesc(t->args[0].get());
            in->text = d->text;
            return in;
        }
        case TypeExpr::Tuple: d->k = ObjTypeDesc::Tuple; return d;
        case TypeExpr::Fn: d->k = ObjTypeDesc::Fn; return d;
        case TypeExpr::Named: break;
    }
    static const std::unordered_map<std::string, ObjTypeDesc::K> names = {
        {"any", ObjTypeDesc::Any},     {"nil", ObjTypeDesc::Nil},     {"void", ObjTypeDesc::Nil},
        {"bool", ObjTypeDesc::Bool},   {"int", ObjTypeDesc::Int},     {"float", ObjTypeDesc::Float},
        {"num", ObjTypeDesc::Num},     {"str", ObjTypeDesc::Str},     {"list", ObjTypeDesc::List},
        {"map", ObjTypeDesc::Map},     {"set", ObjTypeDesc::Set},     {"tuple", ObjTypeDesc::Tuple},
        {"fn", ObjTypeDesc::Fn},       {"range", ObjTypeDesc::Range}, {"flow", ObjTypeDesc::Flow},
        {"task", ObjTypeDesc::Task},   {"vec", ObjTypeDesc::Vec},     {"mat", ObjTypeDesc::Mat},
        {"quat", ObjTypeDesc::Quat},   {"rng", ObjTypeDesc::Rng},     {"time", ObjTypeDesc::Time},
        {"Error", ObjTypeDesc::Error}, {"err", ObjTypeDesc::Error},   {"iter", ObjTypeDesc::Iter},
    };
    auto it = names.find(t->name);
    Decl* named = nullptr;
    if (it == names.end()) {
        auto g = mod.globals.find(t->name);
        if (g != mod.globals.end()) {
            named = g->second;
            if (named->kind == DeclKind::Import && named->importFrom && !named->importFrom->isNative) {
                auto h = named->importFrom->globals.find(named->importName);
                named = h != named->importFrom->globals.end() ? h->second : nullptr;
            }
        }
    }
    if (it != names.end()) {
        d->k = it->second;
    } else if (named && named->kind == DeclKind::Interface && named->node) {
        d->k = ObjTypeDesc::Interface;
        auto* ii = static_cast<InterfaceDecl*>(named->node)->info;
        if (ii)
            for (auto& r : ii->required) d->syms.push_back(vm.sym(r));
    } else if (named && named->kind == DeclKind::Enum) {
        d->k = ObjTypeDesc::EnumOf;
        d->className = t->name;
    } else if (std::isupper((unsigned char)t->name[0]) && !isTypeVar(t->name) && !t->typeVar) {
        d->k = ObjTypeDesc::Class;
        d->className = t->name;
    } else {
        d->k = ObjTypeDesc::Any;
    }
    return d;
}

void Compiler::emitTypeCheck(TypeExpr* t, const std::string& what) {
    if (!t) return;
    ObjTypeDesc* d = typeDesc(t);
    if (d->k == ObjTypeDesc::Any) return;
    emitOp(OP_CHECK_TYPE, 0);
    emitU16(constant(Value::object(d)));
    emitU16(strConst(what));
}

// ------------------------------------------------------------------ module

void Compiler::compileImports(Block* program) {
    for (auto& sp : program->stmts) {
        if (sp->kind != NK::Use) continue;
        auto* u = static_cast<Use*>(sp.get());
        if (!u->module) continue;
        line = u->pos.line;
        if (u->module->isNative) {
            int idx = vm.builtinIndex(u->module->name);
            emitOp(OP_GET_BUILTIN, +1);
            emitU16((uint16_t)idx);
        } else {
            int idx = vm.moduleIndex(u->module->rt);
            emitOp(OP_IMPORT, +1);
            emitU16((uint16_t)idx);
            emitOp(OP_IMPORT_DONE, 0);
            emitU16((uint16_t)idx);
        }
        if (u->moduleDecl) {
            emitOp(OP_DUP, +1);
            emitSet(u->moduleDecl, u->pos);
        }
        for (auto& [d, name] : u->imported) {
            emitOp(OP_DUP, +1);
            emitOp(OP_EXPORT_GET, 0);
            emitU16(strConst(name));
            emitSet(d, u->pos);
        }
        emitOp(OP_POP, -1);
    }
}

ObjFunction* Compiler::compileModule(Block* program, bool isEntry, bool replValue) {
    FnState st;
    st.fn = vm.alloc<ObjFunction>();
    st.fn->name = mod.name == "<main>" ? "<main>" : "<module " + mod.name + ">";
    st.fn->file = mod.path;
    st.fn->module = mod.rt;
    st.info = mod.script;
    st.depth = 1;  // slot 0: the script closure
    fs = &st;

    // make room for new globals (the REPL adds some over time)
    ModuleRt* rt = mod.rt;
    while (rt->globals.size() < mod.globalOrder.size()) {
        rt->globalNames.push_back(mod.globalOrder[rt->globals.size()]->name);
        rt->globals.push_back(Value::undef());
    }
    for (Decl* d : mod.globalOrder)
        if (mod.findExport(d->name) == d) rt->exports[d->name] = d->globalIndex;

    compileImports(program);
    // Hoist top-level functions and classes so code can call them before
    // their definition in the file.
    for (auto& sp : program->stmts) {
        if (sp->kind == NK::FnDecl) {
            auto* f = static_cast<FnDecl*>(sp.get());
            line = f->pos.line;
            ObjFunction* fn = compileFunction(f, f->name);
            (void)fn;
            emitSet(f->decl, f->pos);
        } else if (sp->kind == NK::ClassDecl) {
            auto* c = static_cast<ClassDecl*>(sp.get());
            line = c->pos.line;
            compileClass(c);
            emitSet(c->decl, c->pos);
        } else if (sp->kind == NK::InterfaceDecl) {
            auto* d = static_cast<InterfaceDecl*>(sp.get());
            compileInterface(d);
            emitSet(d->decl, d->pos);
        }
    }
    // enums after classes and interfaces: creating their values may use them
    for (auto& sp : program->stmts) {
        if (sp->kind != NK::EnumDecl) continue;
        auto* e = static_cast<EnumDecl*>(sp.get());
        line = e->pos.line;
        compileEnum(e);
        emitSet(e->decl, e->pos);
    }
    Stmt* lastExpr = nullptr;
    if (replValue && !program->stmts.empty() && program->stmts.back()->kind == NK::ExprStmt)
        lastExpr = program->stmts.back().get();
    for (auto& sp : program->stmts) {
        if (sp->kind == NK::FnDecl || sp->kind == NK::ClassDecl || sp->kind == NK::Use ||
            sp->kind == NK::InterfaceDecl || sp->kind == NK::EnumDecl)
            continue;
        if (sp.get() == lastExpr) {
            line = sp->pos.line;
            auto* es = static_cast<ExprStmt*>(sp.get());
            compileExpr(es->expr.get());
            if (autoCallStatement(es->expr.get())) emitOp(OP_AUTOCALL, 0);
            emitOp(OP_RETURN, -1);
            fs->depth++;  // unreachable, keeps the depth consistent
            emitOp(OP_POP, -1);
            continue;
        }
        compileStmt(sp.get());
    }
    emitExits(0);
    fs->exits.clear();
    if (isEntry) {
        auto it = mod.globals.find("main");
        if (it != mod.globals.end() && it->second->kind == DeclKind::Fn && it->second->node) {
            auto* mf = static_cast<FnDecl*>(it->second->node);
            bool required = false;
            for (auto& p : mf->params)
                if (!p.def) required = true;
            if (!required) {
                emitGet(it->second, mf->pos);
                emitOp(OP_CALL, 0);
                emitByte(0);
                emitOp(OP_POP, -1);
            }
        }
    }
    emitOp(OP_NIL, +1);
    emitOp(OP_RETURN, -1);
    fs = nullptr;
    return st.fn;
}

// ------------------------------------------------------------------ functions

void Compiler::emitClosure(ObjFunction* fn, FnState& st) {
    emitOp(OP_CLOSURE, +1);
    emitU16(constant(Value::object(fn)));
    for (auto& u : st.upvals) {
        emitByte(u.isLocal ? 1 : 0);
        emitU16(u.index);
    }
}

ObjFunction* Compiler::compileFunction(FnDecl* fnd, const std::string& qualName) {
    FuncInfo* info = fnd->info;
    FnState st;
    st.enclosing = fs;
    st.info = info;
    st.cls = info ? info->cls : nullptr;
    auto* fn = vm.alloc<ObjFunction>();
    st.fn = fn;
    fn->name = qualName;
    fn->file = mod.path;
    fn->module = mod.rt;
    fn->lenient = fnd->lenientArity;
    fn->isFork = fnd->isFork;
    fn->isPar = fnd->isPar;
    fn->methodStyle = fnd->isMethod || fnd->selfFirst;
    fn->exported = fnd->exported || !fnd->isMethod;
    int savedLine = line;
    line = fnd->pos.line;
    fs = &st;

    // slot 0: self for methods, otherwise the callee
    if (info && info->selfDecl) bindSlot(st, info->selfDecl, 0);
    st.depth = 1;
    int arity = 0, minArity = 0;
    bool sawDefault = false;
    size_t firstParam = fnd->selfFirst ? 1 : 0;
    for (size_t i = firstParam; i < fnd->params.size(); i++) {
        auto& p = fnd->params[i];
        arity++;
        if (p.def) {
            sawDefault = true;
        } else {
            if (sawDefault) error(p.pos, "parameter '" + p.name + "' needs a default value because an earlier one has one");
            minArity = arity;
        }
        if (p.decl) bindSlot(st, p.decl, st.depth);
        fn->paramNames.push_back(p.name);
        st.depth++;
    }
    fn->arity = arity;
    fn->minArity = minArity;
    fn->autoCall = fnAutoCalls(fnd);
    // sort(_a < _b), sort(_b.age < _a.age): a shape sort can compare directly
    if (fnd->isLambda && fnd->params.size() == 2 && fnd->body && fnd->body->stmts.size() == 1 &&
        fnd->body->stmts[0]->kind == NK::ExprStmt) {
        Expr* e = static_cast<ExprStmt*>(fnd->body->stmts[0].get())->expr.get();
        if (e->kind == NK::Binary) {
            auto* b = static_cast<Binary*>(e);
            int op = b->op == BinOp::Lt ? 0 : b->op == BinOp::Le ? 1 : b->op == BinOp::Gt ? 2 : b->op == BinOp::Ge ? 3 : -1;
            // which parameter a side reads, and through which field
            auto side = [&](Expr* x, int& param, std::string& field) {
                field.clear();
                if (x->kind == NK::Member && !static_cast<Member*>(x)->isCall) {
                    field = static_cast<Member*>(x)->name;
                    x = static_cast<Member*>(x)->object.get();
                }
                param = -1;
                if (x->kind != NK::Ident) return;
                Decl* d = static_cast<Ident*>(x)->decl;
                if (d && d == fnd->params[0].decl) param = 0;
                if (d && d == fnd->params[1].decl) param = 1;
            };
            int pl, pr;
            std::string fl, fr;
            side(b->left.get(), pl, fl);
            side(b->right.get(), pr, fr);
            if (op >= 0 && pl >= 0 && pr >= 0 && pl != pr && fl == fr && !fnd->params[0].type && !fnd->params[1].type) {
                fn->cmpOp = (int8_t)op;
                fn->cmpSwap = pl == 1;
                fn->cmpField = fl.empty() ? -1 : vm.sym(fl);
            }
        }
    }
    // sort_by(_.age): a key that is one field of the argument
    if (fnd->isLambda && fnd->params.size() == 1 && !fnd->params[0].type && fnd->body && fnd->body->stmts.size() == 1 &&
        fnd->body->stmts[0]->kind == NK::ExprStmt) {
        Expr* e = static_cast<ExprStmt*>(fnd->body->stmts[0].get())->expr.get();
        if (e->kind == NK::Member && !static_cast<Member*>(e)->isCall) {
            auto* mem = static_cast<Member*>(e);
            Expr* o = mem->object.get();
            if (o->kind == NK::Ident && static_cast<Ident*>(o)->decl && static_cast<Ident*>(o)->decl == fnd->params[0].decl)
                fn->keyField = vm.sym(mem->name);
        }
    }
    if (std::find(fnd->attrs.begin(), fnd->attrs.end(), "$nojit") != fnd->attrs.end()) fn->noJit = true;
    if (info) {
        for (Decl* h : info->hoisted) {
            emitOp(OP_UNDEF, +1);
            bindSlot(st, h, st.depth - 1);
        }
    }
    if (fnd->isGpu) {
        // a gpu fn: its kernel is checked and made now; its body here just launches it
        std::string err;
        int el = 0, ec = 0;
        int kid = gpuCompile(fnd, err, el, ec);
        if (kid < 0) error(SrcPos{el ? el : fnd->pos.line, el ? ec : fnd->pos.col}, "gpu fn " + fnd->name + ": " + err);
        fn->gpuKernel = kid;
        emitOp(OP_GET_BUILTIN, +1);
        emitU16((uint16_t)vm.builtinIndex("__gpu_launch"));
        emitOp(OP_CONST, +1);
        emitU16(constant(Value::integer(kid)));
        int argc = 0;
        for (size_t i = firstParam; i < fnd->params.size(); i++) {
            emitOp(OP_GET_LOCAL, +1);
            emitU16((uint16_t)(fnd->params[i].decl ? st.slotOf[fnd->params[i].decl] : 0));
            argc++;
        }
        emitOp(OP_CALL, -(argc + 1));
        emitByte((uint8_t)(argc + 1));
        emitOp(OP_RETURN, -1);
        fn->upvalueCount = (int)st.upvals.size();
        fs = st.enclosing;
        line = savedLine;
        emitClosure(fn, st);
        return fn;
    }
    // default arguments and runtime checks of annotated parameters
    for (size_t i = firstParam; i < fnd->params.size(); i++) {
        auto& p = fnd->params[i];
        if (!p.decl) continue;
        size_t slot = st.slotOf[p.decl];
        if (p.def) {
            emitOp(OP_JUMP_IF_SET, 0);
            emitU16((uint16_t)slot);
            size_t at = chunk().code.size();
            emitU32(0);
            compileExpr(p.def.get());
            emitOp(OP_SET_LOCAL, -1);
            emitU16((uint16_t)slot);
            patchJump(at);
        }
        if (p.type) {
            ObjTypeDesc* d = typeDesc(p.type.get());
            if (d->k != ObjTypeDesc::Any) {
                emitOp(OP_GET_LOCAL, +1);
                emitU16((uint16_t)slot);
                emitOp(OP_CHECK_TYPE, 0);
                emitU16(constant(Value::object(d)));
                emitU16(strConst("argument '" + p.name + "' of " + qualName));
                emitOp(OP_SET_LOCAL, -1);
                emitU16((uint16_t)slot);
            }
        }
    }
    if (fnd->ret) {
        ObjTypeDesc* d = typeDesc(fnd->ret.get());
        if (d->k != ObjTypeDesc::Any) st.retCheck = d;
    }
    compileBlock(fnd->body.get(), true);
    if (st.retCheck) {
        emitOp(OP_CHECK_TYPE, 0);
        emitU16(constant(Value::object(st.retCheck)));
        emitU16(strConst("return value of " + qualName));
    }
    emitOp(OP_RETURN, -1);

    fn->upvalueCount = (int)st.upvals.size();
    for (auto& u : st.upvals) fn->upvalueNames.push_back(u.decl ? u.decl->name : "?");
    if (std::find(fnd->attrs.begin(), fnd->attrs.end(), "$jit") != fnd->attrs.end() ||
        std::find(fnd->attrs.begin(), fnd->attrs.end(), "$hot") != fnd->attrs.end()) {
        fn->wantJit = true;
        if (jitSupported()) {
            std::string why = jitCheck(fn);
            if (!why.empty())
                diags.warning(mod.path, fnd->namePos, "$jit: " + fnd->name + " can't be compiled to machine code: it " + why,
                              (int)fnd->name.size());
        }
    }
    fs = st.enclosing;
    line = savedLine;
    emitClosure(fn, st);
    return fn;
}

void Compiler::compileClass(ClassDecl* c) {
    ClassInfo* ci = c->info;
    auto* tmpl = vm.alloc<ObjClass>();
    tmpl->name = c->name;
    tmpl->module = mod.rt;
    if (ci) {
        tmpl->fields = ci->fields;
        for (size_t i = 0; i < ci->fields.size(); i++) tmpl->fieldIndex[vm.sym(ci->fields[i])] = (int)i;
        tmpl->positional = ci->positional;
    }
    emitOp(OP_CLASS, +1);
    emitU16(constant(Value::object(tmpl)));
    for (auto& m : c->methods) {
        compileFunction(m.get(), c->name + "." + m->name);
        emitOp(OP_METHOD, -1);
        emitU16((uint16_t)vm.sym(m->name));
        emitByte(m->exported ? 1 : 0);
    }
    // methods from interfaces (defaults) and enums, unless the class has its own
    for (FnDecl* fn : c->shared) {
        bool own = false;
        for (auto& m : c->methods)
            if (m->name == fn->name) own = true;
        if (own) continue;
        compileFunction(fn, c->name + "." + fn->name);
        emitOp(OP_METHOD, -1);
        emitU16((uint16_t)vm.sym(fn->name));
        emitByte(1);
    }
    ObjFunction* ctor = compileCtor(c);
    tmpl->ctorArity = ctor->arity;
    tmpl->ctorMinArity = ctor->minArity;
    emitOp(OP_CTOR, -1);
}

void Compiler::compileInterface(InterfaceDecl* d) {
    auto* iface = vm.alloc<ObjInterface>();
    iface->name = d->name;
    if (d->info)
        for (auto& r : d->info->required) iface->required.push_back(vm.sym(r));
    emitConst(Value::object(iface));
}

// An enum is a namespace of variants: data variants are classes, the others
// are single instances.
void Compiler::compileEnum(EnumDecl* e) {
    for (auto& v : e->variants) {
        compileClass(v.cls.get());
        if (v.unit) {
            emitOp(OP_CALL, 0);
            emitByte(0);
        }
    }
    emitOp(OP_MAKE_NS, 1 - (int)e->variants.size());
    emitU16(strConst(e->name));
    emitU16((uint16_t)e->variants.size());
    for (auto& v : e->variants) emitU16(strConst(v.name));
}

ObjFunction* Compiler::compileCtor(ClassDecl* c) {
    ClassInfo* ci = c->info;
    FuncInfo* info = ci->ctorInfo;
    FnState st;
    st.enclosing = fs;
    st.info = info;
    st.cls = ci;
    auto* fn = vm.alloc<ObjFunction>();
    st.fn = fn;
    fn->name = c->name;
    fn->file = mod.path;
    fn->module = mod.rt;
    fn->methodStyle = true;
    fn->isCtor = true;
    int savedLine = line;
    line = c->pos.line;
    fs = &st;
    bindSlot(st, info->selfDecl, 0);
    st.depth = 1;
    for (Decl* p : info->params) {
        bindSlot(st, p, st.depth++);
        fn->paramNames.push_back(p->name);
    }
    // a class with `init`: its constructor takes init's parameters (for named arguments)
    if (!c->hasCtorParams && c->info && c->info->init && c->info->init->params.size() == fn->paramNames.size())
        for (size_t i = 0; i < fn->paramNames.size(); i++) fn->paramNames[i] = c->info->init->params[i].name;
    fn->arity = (int)info->params.size();
    if (c->hasCtorParams) {
        int minA = 0;
        for (size_t i = 0; i < c->ctorParams.size(); i++)
            if (!c->ctorParams[i].def) minA = (int)i + 1;
        fn->minArity = minA;
        for (auto& p : c->ctorParams) {
            size_t slot = st.slotOf[p.decl];
            if (p.def) {
                emitOp(OP_JUMP_IF_SET, 0);
                emitU16((uint16_t)slot);
                size_t at = chunk().code.size();
                emitU32(0);
                compileExpr(p.def.get());
                emitOp(OP_SET_LOCAL, -1);
                emitU16((uint16_t)slot);
                patchJump(at);
            }
            if (p.type) {
                emitOp(OP_GET_LOCAL, +1);
                emitU16((uint16_t)slot);
                emitTypeCheck(p.type.get(), "argument '" + p.name + "' of " + c->name);
                emitOp(OP_SET_LOCAL, -1);
                emitU16((uint16_t)slot);
            }
        }
    } else if (ci->init) {
        int minA = 0;
        for (size_t i = 0; i < ci->init->params.size(); i++)
            if (!ci->init->params[i].def) minA = (int)i + 1;
        fn->minArity = minA;
    }
    // field defaults (evaluated for every new instance)
    for (auto& fd : c->fields) {
        if (!fd.def) continue;
        line = fd.pos.line;
        emitOp(OP_GET_LOCAL, +1);
        emitU16(0);
        compileExpr(fd.def.get());
        if (fd.type) emitTypeCheck(fd.type.get(), "field '" + fd.name + "' of " + c->name);
        emitOp(OP_SET_FIELD, -2);
        emitU16((uint16_t)ci->fieldIndex[fd.name]);
    }
    if (c->hasCtorParams) {
        for (auto& p : c->ctorParams) {
            emitOp(OP_GET_LOCAL, +1);
            emitU16(0);
            emitOp(OP_GET_LOCAL, +1);
            emitU16((uint16_t)st.slotOf[p.decl]);
            emitOp(OP_SET_FIELD, -2);
            emitU16((uint16_t)ci->fieldIndex[p.name]);
        }
    }
    if (ci->init) {
        emitOp(OP_GET_LOCAL, +1);
        emitU16(0);
        int argc = 0;
        if (!c->hasCtorParams) {
            // forward the constructor arguments to init
            for (Decl* p : info->params) {
                emitOp(OP_GET_LOCAL, +1);
                emitU16((uint16_t)st.slotOf[p]);
                argc++;
            }
        }
        emitOp(OP_INVOKE, -argc);
        emitU16((uint16_t)vm.sym("init"));
        emitByte((uint8_t)argc);
        emitByte(FB_NONE);
        emitU16(0);
        emitIC();
        emitOp(OP_POP, -1);
    }
    emitOp(OP_GET_LOCAL, +1);
    emitU16(0);
    emitOp(OP_RETURN, -1);
    fn->upvalueCount = (int)st.upvals.size();
    for (auto& u : st.upvals) fn->upvalueNames.push_back(u.decl ? u.decl->name : "?");
    fs = st.enclosing;
    line = savedLine;
    emitClosure(fn, st);
    return fn;
}

// ------------------------------------------------------------------ statements

void Compiler::compileBlock(Block* b, bool wantValue) {
    beginScope();
    if (b) {
        for (size_t i = 0; i < b->stmts.size(); i++) {
            Stmt* s = b->stmts[i].get();
            bool last = i + 1 == b->stmts.size();
            if (last && wantValue) {
                line = s->pos.line;
                if (s->kind == NK::ExprStmt) {
                    Expr* e = static_cast<ExprStmt*>(s)->expr.get();
                    if (e->kind == NK::If) {
                        compileIf(static_cast<If*>(e), true);
                    } else if (e->kind == NK::Match) {
                        compileMatch(static_cast<Match*>(e), true);
                    } else {
                        compileExpr(e);
                        if (autoCallStatement(e)) emitOp(OP_AUTOCALL, 0);
                    }
                } else if (s->kind == NK::Region) {
                    compileRegion(static_cast<Region*>(s), true);
                } else {
                    compileStmt(s);
                    emitOp(OP_NIL, +1);
                }
            } else {
                compileStmt(s);
            }
        }
    }
    if (wantValue && (!b || b->stmts.empty())) emitOp(OP_NIL, +1);
    endScope(wantValue);
}

void Compiler::compileAssign(Assign* a) {
    Expr* t = a->target.get();
    if (a->op == BinOp::None) {
        if (t->kind == NK::Ident && a->value->kind == NK::Binary && growsGlobal(static_cast<Ident*>(t)->decl)) {
            auto* b = static_cast<Binary*>(a->value.get());
            Decl* d = static_cast<Ident*>(t)->decl;
            if (b->op == BinOp::Add && b->left->kind == NK::Ident && static_cast<Ident*>(b->left.get())->decl == d) {
                compileExpr(b->right.get());  // g = g + x (reading g in x seals it first)
                emitOp(OP_STR_APPEND_G, -1);
                emitU16((uint16_t)d->globalIndex);
                emitByte(1);
                fs->fn->globalsUsed.push_back((uint16_t)d->globalIndex);
                return;
            }
        }
        if (t->kind == NK::Ident && strAppends.count(a)) {  // s = s + x, s being a loop's own string
            auto* id = static_cast<Ident*>(t);
            compileExpr(static_cast<Binary*>(a->value.get())->right.get());
            emitOp(OP_STR_APPEND, -1);
            emitU16((uint16_t)fs->slotOf[id->decl]);
            emitByte(1);
            return;
        }
        if (t->kind == NK::Ident) {
            auto* id = static_cast<Ident*>(t);
            compileExpr(a->value.get());
            TypeExpr* annot = a->annot ? a->annot.get() : (id->decl ? id->decl->annot : nullptr);
            if (annot) emitTypeCheck(annot, "variable '" + id->name + "'");
            emitSet(id->decl, id->pos);
        } else if (t->kind == NK::Member) {
            auto* m = static_cast<Member*>(t);
            compileExpr(m->object.get());
            compileExpr(a->value.get());
            bool selfField = m->object->kind == NK::Ident && static_cast<Ident*>(m->object.get())->name == "self" &&
                             fs->cls && fs->cls->fieldIndex.count(m->name);
            if (selfField) {
                TypeExpr* ft = fs->cls->fieldTypes[fs->cls->fieldIndex[m->name]];
                if (ft) emitTypeCheck(ft, "field '" + m->name + "'");
                emitOp(OP_SET_FIELD, -2);
                emitU16((uint16_t)fs->cls->fieldIndex[m->name]);
            } else {
                emitOp(OP_SET_MEMBER, -2);
                emitU16((uint16_t)vm.sym(m->name));
                emitIC();
            }
        } else if (t->kind == NK::Index) {
            auto* ix = static_cast<Index*>(t);
            compileExpr(ix->object.get());
            compileExpr(ix->index.get());
            compileExpr(a->value.get());
            emitOp(OP_SET_INDEX, -3);
        }
        return;
    }
    Op op;
    switch (a->op) {
        case BinOp::Add: op = OP_ADD; break;
        case BinOp::Sub: op = OP_SUB; break;
        case BinOp::Mul: op = OP_MUL; break;
        case BinOp::Div: op = OP_DIV; break;
        case BinOp::Mod: op = OP_MOD; break;
        case BinOp::Pow: op = OP_POW; break;
        default: op = OP_ADD; break;
    }
    // a += b: INPLACE (an object whose class defines `+=` changes itself)
    auto emitArith = [&](Op o) {
        if (o == OP_POW) {
            emitOp(o, -1);
            return;
        }
        emitOp(OP_INPLACE, -1);
        emitByte((uint8_t)o);
    };
    if (t->kind == NK::Ident) {
        auto* id = static_cast<Ident*>(t);
        // fast path: local += small int constant
        if ((a->op == BinOp::Add || a->op == BinOp::Sub) && a->value->kind == NK::IntLit && id->decl &&
            !id->decl->annot && !id->decl->isGlobal && fs->slotOf.count(id->decl)) {
            int64_t k = static_cast<IntLit*>(a->value.get())->value;
            if (a->op == BinOp::Sub) k = -k;
            if (k >= -128 && k <= 127) {
                emitOp(OP_INC_LOCAL, 0);
                emitU16((uint16_t)fs->slotOf[id->decl]);
                emitByte((uint8_t)(int8_t)k);
                return;
            }
        }
        if (a->op == BinOp::Add && growsGlobal(id->decl) && a->value->kind != NK::IntLit) {
            compileExpr(a->value.get());
            emitOp(OP_STR_APPEND_G, -1);
            emitU16((uint16_t)id->decl->globalIndex);
            emitByte(0);
            fs->fn->globalsUsed.push_back((uint16_t)id->decl->globalIndex);
            return;
        }
        if (a->op == BinOp::Add && strAppends.count(a)) {  // a loop's own string grows in place
            compileExpr(a->value.get());
            emitOp(OP_STR_APPEND, -1);
            emitU16((uint16_t)fs->slotOf[id->decl]);
            emitByte(0);
            return;
        }
        emitGet(id->decl, id->pos);
        compileExpr(a->value.get());
        emitArith(op);
        if (id->decl && id->decl->annot) emitTypeCheck(id->decl->annot, "variable '" + id->name + "'");
        emitSet(id->decl, id->pos);
    } else if (t->kind == NK::Member && m_selfField(static_cast<Member*>(t))) {
        // self.x += v (or x += v for a field in a method): its fixed slot
        auto* m = static_cast<Member*>(t);
        uint16_t slot = (uint16_t)fs->cls->fieldIndex[m->name];
        compileExpr(m->object.get());
        emitOp(OP_DUP, +1);
        emitOp(OP_GET_FIELD, 0);
        emitU16(slot);
        compileExpr(a->value.get());
        emitArith(op);
        TypeExpr* ft = fs->cls->fieldTypes[slot];
        if (ft) emitTypeCheck(ft, "field '" + m->name + "'");
        emitOp(OP_SET_FIELD, -2);
        emitU16(slot);
    } else if (t->kind == NK::Member) {
        auto* m = static_cast<Member*>(t);
        compileExpr(m->object.get());
        emitOp(OP_DUP, +1);
        emitOp(OP_GET_MEMBER_RAW, 0);
        emitU16((uint16_t)vm.sym(m->name));
        emitByte(FB_NONE);
        emitU16(0);
        emitIC();
        compileExpr(a->value.get());
        emitArith(op);
        emitOp(OP_SET_MEMBER, -2);
        emitU16((uint16_t)vm.sym(m->name));
        emitIC();
    } else if (t->kind == NK::Index) {
        auto* ix = static_cast<Index*>(t);
        compileExpr(ix->object.get());
        compileExpr(ix->index.get());
        emitOp(OP_DUP2, +2);
        emitOp(OP_GET_INDEX, -1);
        compileExpr(a->value.get());
        emitArith(op);
        emitOp(OP_SET_INDEX, -3);
    }
}

void Compiler::compileStmt(Stmt* s) {
    line = s->pos.line;
    switch (s->kind) {
        case NK::ExprStmt: {
            Expr* e = static_cast<ExprStmt*>(s)->expr.get();
            if (e->kind == NK::StrLit && static_cast<StrLit*>(e)->docString) return;
            if (e->kind == NK::If) {
                compileIf(static_cast<If*>(e), false);
                return;
            }
            if (e->kind == NK::Match) {
                compileMatch(static_cast<Match*>(e), false);
                return;
            }
            compileExpr(e);
            if (autoCallStatement(e)) emitOp(OP_AUTOCALL, 0);
            emitOp(OP_POP, -1);
            return;
        }
        case NK::Assign: {
            SelfTargetSwap sw;
            sw.in(static_cast<Assign*>(s)->target);
            compileAssign(static_cast<Assign*>(s));
            return;
        }
        case NK::Destructure: {
            auto* d = static_cast<Destructure*>(s);
            SelfTargetSwap sw;
            for (auto& t : d->targets) sw.in(t);
            compileExpr(d->value.get());
            emitOp(OP_DESTRUCT, (int)d->targets.size() - 1);
            emitByte((uint8_t)d->targets.size());
            for (size_t i = d->targets.size(); i-- > 0;) {
                Expr* t = d->targets[i].get();
                if (t->kind == NK::Ident) {
                    emitSet(static_cast<Ident*>(t)->decl, t->pos);
                } else if (t->kind == NK::Member) {
                    auto* m = static_cast<Member*>(t);
                    compileExpr(m->object.get());
                    emitOp(OP_SWAP, 0);
                    emitOp(OP_SET_MEMBER, -2);
                    emitU16((uint16_t)vm.sym(m->name));
                    emitIC();
                } else if (t->kind == NK::Index) {
                    auto* ix = static_cast<Index*>(t);
                    compileExpr(ix->object.get());
                    compileExpr(ix->index.get());
                    emitOp(OP_ROT3, 0);
                    emitOp(OP_SET_INDEX, -3);
                }
            }
            return;
        }
        case NK::VarDecl: {
            auto* v = static_cast<VarDecl*>(s);
            if (v->init)
                compileExpr(v->init.get());
            else
                emitOp(OP_NIL, +1);
            if (v->annot) emitTypeCheck(v->annot.get(), "variable '" + v->name + "'");
            if (v->decl && v->decl->isGlobal)
                emitSet(v->decl, v->pos);
            else
                addLocal(v->decl);
            return;
        }
        case NK::IncDec: {
            auto* x = static_cast<IncDec*>(s);
            SelfTargetSwap sw;
            sw.in(x->target);
            Assign tmp(x->pos, nullptr, nullptr);
            tmp.op = x->inc ? BinOp::Add : BinOp::Sub;
            tmp.target = std::move(x->target);
            tmp.value = std::make_unique<IntLit>(x->pos, 1);
            compileAssign(&tmp);
            x->target = std::move(tmp.target);
            return;
        }
        case NK::Block: compileBlock(static_cast<Block*>(s), false); return;
        case NK::While: {
            auto* w = static_cast<While*>(s);
            if (w->jitHint && fs->fn) fs->fn->wantJit = true;
            prepareStrBuilders({w->cond.get(), w->body.get()});
            size_t start = chunk().code.size();
            size_t exitJ = compileCondJump(w->cond.get());
            fs->loops.push_back({start, fs->depth, {}, fs->exits.size()});
            compileBlock(w->body.get(), false);
            emitLoop(start);
            patchJump(exitJ);
            for (size_t j : fs->loops.back().breakJumps) patchJump(j);
            fs->loops.pop_back();
            return;
        }
        case NK::For: compileFor(static_cast<For*>(s)); return;
        case NK::ForRecv: compileForRecv(static_cast<ForRecv*>(s)); return;
        case NK::Return: {
            auto* r = static_cast<Return*>(s);
            if (r->value)
                compileExpr(r->value.get());
            else
                emitOp(OP_NIL, +1);
            if (fs->retCheck) {
                emitOp(OP_CHECK_TYPE, 0);
                emitU16(constant(Value::object(fs->retCheck)));
                emitU16(strConst("return value of " + fs->fn->name));
            }
            emitExits(0);
            emitOp(OP_RETURN, -1);
            return;
        }
        case NK::Break: {
            if (fs->loops.empty()) return;
            size_t saved = fs->depth;
            emitExits(fs->loops.back().exitLevel);
            unwindTo(fs->loops.back().depth);
            fs->loops.back().breakJumps.push_back(emitJump(OP_JUMP, 0));
            fs->depth = saved;
            return;
        }
        case NK::Continue: {
            if (fs->loops.empty()) return;
            size_t saved = fs->depth;
            emitExits(fs->loops.back().exitLevel);
            unwindTo(fs->loops.back().depth);
            emitLoop(fs->loops.back().continueTarget);
            fs->depth = saved;
            return;
        }
        case NK::Retry: {
            auto* r = static_cast<Retry*>(s);
            if (fs->retries.empty()) return;
            RetryCtx ctx = fs->retries.back();
            size_t skip = 0;
            bool hasSkip = false;
            if (r->count) {
                emitOp(OP_GET_LOCAL, +1);
                emitU16((uint16_t)ctx.attemptSlot);
                compileExpr(r->count.get());
                emitOp(OP_LT, -1);
                skip = emitJump(OP_JUMP_IF_FALSE, -1);
                hasSkip = true;
            }
            size_t saved = fs->depth;
            emitOp(OP_GET_LOCAL, +1);
            emitU16((uint16_t)ctx.attemptSlot);
            emitConst(Value::integer(1));
            emitOp(OP_ADD, -1);
            emitOp(OP_SET_LOCAL, -1);
            emitU16((uint16_t)ctx.attemptSlot);
            if (r->delay) {
                compileExpr(r->delay.get());
                emitOp(OP_SLEEP, -1);
            }
            emitExits(ctx.exitLevel);
            unwindTo(ctx.depth);
            emitLoop(ctx.loopStart);
            fs->depth = saved;
            if (hasSkip) patchJump(skip);
            return;
        }
        case NK::FnDecl: {
            auto* f = static_cast<FnDecl*>(s);
            compileFunction(f, f->name);
            emitSet(f->decl, f->pos);
            return;
        }
        case NK::ClassDecl: {
            auto* c = static_cast<ClassDecl*>(s);
            compileClass(c);
            emitSet(c->decl, c->pos);
            return;
        }
        case NK::Use: return;
        case NK::With: {
            beginScope();
            emitOp(OP_WITH_BEGIN, 0);
            fs->exits.push_back({ExitAction::WithEnd, nullptr, fs->scopeStarts.size()});
            compileBlock(static_cast<With*>(s)->body.get(), false);
            endScope(false);
            return;
        }
        case NK::Region: compileRegion(static_cast<Region*>(s), false); return;
        case NK::Select: compileSelect(static_cast<Select*>(s)); return;
        case NK::InterfaceDecl: {
            auto* d = static_cast<InterfaceDecl*>(s);
            compileInterface(d);
            emitSet(d->decl, d->pos);
            return;
        }
        case NK::EnumDecl: {
            auto* e = static_cast<EnumDecl*>(s);
            compileEnum(e);
            emitSet(e->decl, e->pos);
            return;
        }
        case NK::Defer:
            fs->exits.push_back({ExitAction::DeferBlock, static_cast<Defer*>(s)->body.get(), fs->scopeStarts.size()});
            return;
        case NK::Recv: {
            auto* r = static_cast<Recv*>(s);
            compileExpr(r->chan.get());
            emitOp(OP_RECV, 0);
            emitSet(static_cast<Ident*>(r->target.get())->decl, r->pos);
            return;
        }
        case NK::Todo: {
            auto* t = static_cast<Todo*>(s);
            emitOp(OP_TODO, 0);
            emitU16(strConst(t->message));
            return;
        }
        default: error(s->pos, "internal: can't compile statement"); return;
    }
}

// @arena / @frame / ... blocks free what they allocated when they end;
// @cache / @dual blocks get a persistent object named by the block.
void Compiler::compileRegion(Region* r, bool wantValue) {
    beginScope();
    if (r->nameDecl) {
        // the object is created the first time this block runs
        emitOp(OP_GET_GLOBAL_OPT, +1);
        emitU16((uint16_t)r->storeDecl->globalIndex);
        size_t haveIt = emitJump(OP_JUMP_IF_DEFINED, 0);
        emitOp(OP_POP, -1);
        emitOp(OP_GET_BUILTIN, +1);
        emitU16((uint16_t)vm.builtinIndex(r->kind == "dual" ? "Dual" : "Cache"));
        int argc = 0;
        if (r->kind == "dual" && r->init) {
            compileExpr(r->init.get());
            compileExpr(r->init.get());
            argc = 2;
        }
        emitOp(OP_CALL, -argc);
        emitByte((uint8_t)argc);
        emitOp(OP_DUP, +1);
        emitSet(r->storeDecl, r->pos);
        patchJump(haveIt);
        addLocal(r->nameDecl);
    }
    if (r->isArena()) {
        emitOp(OP_REGION_BEGIN, 0);
        fs->exits.push_back({ExitAction::RegionEnd, nullptr, fs->scopeStarts.size()});
    }
    // the block's own variables go away before the region ends, so what they
    // held can be freed
    beginScope();
    for (Decl* d : r->scopedVars) {
        emitOp(OP_UNDEF, +1);
        addLocal(d);
    }
    compileBlock(r->body.get(), wantValue);
    endScope(wantValue);
    endScope(wantValue);
}

void Compiler::compileFor(For* f) {
    if (f->jitHint && fs->fn) fs->fn->wantJit = true;
    if (f->step) {
        if (f->iter->kind != NK::Range) {
            error(f->step->pos, "a step (', n') only works with a range like 0..<10");
        } else {
            auto* r = static_cast<Range*>(f->iter.get());
            r->step = std::move(f->step);
        }
    }
    // for i, x in list / for k, v in map / for i, (a, b) in pairs
    bool keyed = (f->vars.size() == 2 && !f->destructure) || f->unpackItem;
    if (f->iter->kind == NK::Range && f->vars.size() == 1) {
        auto* r = static_cast<Range*>(f->iter.get());
        if (r->start && r->end) {
            compileForRange(f, r);
            return;
        }
    }
    compileExpr(f->iter.get());
    if (!keyed && f->vars.size() == 1) {
        compileForSeq(f);
        return;
    }
    emitOp(OP_ITER_INIT, 0);
    emitByte(keyed ? 2 : f->vars.size() >= 2 ? 1 : 0);
    size_t iterSlot = fs->depth - 1;
    size_t iterDepth = fs->depth;
    prepareStrBuilders({f->body.get()});
    size_t start = chunk().code.size();
    emitOp(OP_FOR_NEXT, +1);
    emitU16((uint16_t)iterSlot);
    size_t exitJ = chunk().code.size();
    emitU32(0);
    fs->loops.push_back({start, iterDepth, {}, fs->exits.size()});
    beginScope();
    fs->scopeStarts.back() = iterDepth;
    if (f->vars.size() == 1) {
        addLocal(f->vars[0].decl);
    } else if (keyed) {
        // the item is on the stack (unpacked if asked); the position (or key) goes on top
        size_t items = 1;
        if (f->unpackItem) {
            items = f->vars.size() - 1;
            emitOp(OP_DESTRUCT, (int)items - 1);
            emitByte((uint8_t)items);
        }
        emitOp(OP_ITER_KEY, +1);
        emitU16((uint16_t)iterSlot);
        for (size_t k = 0; k < items; k++) bindSlot(*fs, f->vars[1 + k].decl, iterDepth + k);
        bindSlot(*fs, f->vars[0].decl, iterDepth + items);
    } else {
        emitOp(OP_DESTRUCT, (int)f->vars.size() - 1);
        emitByte((uint8_t)f->vars.size());
        for (size_t i = 0; i < f->vars.size(); i++) bindSlot(*fs, f->vars[i].decl, iterDepth + i);
    }
    compileBlock(f->body.get(), false);
    endScope(false);
    emitLoop(start);
    fs->depth = iterDepth;
    patchJump(exitJ);
    for (size_t j : fs->loops.back().breakJumps) patchJump(j);
    fs->loops.pop_back();
    emitOp(OP_ENDSCOPE, -1);
    emitU16(1);
}

// g += x on a string module variable of this module (or one the checker can't
// type): grows in place until something reads g.
bool Compiler::growsGlobal(Decl* d) {
    if (!d || !d->isGlobal || d->kind == DeclKind::Builtin || d->annot || d->globalIndex < 0 || d->globalIndex >= 0xFFFF || d->importFrom) return false;
    if (d->module && d->module != &mod) return false;
    return !d->type || d->type->k == Type::Str || d->type->k == Type::Any;
}

// Locals a loop only grows with `s += x` (or `s = s + x`) and never reads
// otherwise: nothing else can see their string while the loop runs, so it can
// grow in place. Marks those assignments and seals the strings at loop entry.
void Compiler::prepareStrBuilders(std::initializer_list<Node*> parts) {
    std::unordered_map<Decl*, int> uses, growths;
    std::vector<Assign*> found;
    auto isLocal = [&](Decl* d) {
        // a string, or a type the checker doesn't know (not numbers: they add as they are)
        bool maybeStr = !d || !d->type || d->type->k == Type::Str || d->type->k == Type::Any;
        return d && maybeStr && !d->isGlobal && !d->captured && !d->annot && fs->slotOf.count(d) &&
               fs->slotOf[d] < 0xFFFF;
    };
    auto mentions = [&](Node* n, Decl* d) {
        bool hit = false;
        std::function<void(Node*)> walk = [&](Node* x) {
            if (!x || hit) return;
            if (x->kind == NK::Ident && static_cast<Ident*>(x)->decl == d) hit = true;
            forEachChild(x, walk);
        };
        walk(n);
        return hit;
    };
    std::function<void(Node*)> walk = [&](Node* x) {
        if (!x) return;
        if (x->kind == NK::Ident) uses[static_cast<Ident*>(x)->decl]++;
        if (x->kind == NK::Assign) {
            auto* a = static_cast<Assign*>(x);
            if (a->target->kind == NK::Ident && !a->annot) {
                Decl* d = static_cast<Ident*>(a->target.get())->decl;
                if (isLocal(d)) {
                    if (a->op == BinOp::Add && a->value->kind != NK::IntLit && !mentions(a->value.get(), d)) {
                        growths[d] += 1;  // the target
                        found.push_back(a);
                    } else if (a->op == BinOp::None && a->value->kind == NK::Binary) {
                        auto* b = static_cast<Binary*>(a->value.get());
                        if (b->op == BinOp::Add && b->left->kind == NK::Ident &&
                            static_cast<Ident*>(b->left.get())->decl == d && !mentions(b->right.get(), d)) {
                            growths[d] += 2;  // the target and the left operand
                            found.push_back(a);
                        }
                    }
                }
            }
        }
        forEachChild(x, walk);
    };
    for (Node* p : parts) walk(p);
    std::vector<Decl*> sealed;
    for (Assign* a : found) {
        Decl* d = static_cast<Ident*>(a->target.get())->decl;
        if (uses[d] != growths[d]) continue;  // read somewhere in the loop: not only grown
        strAppends.insert(a);
        if (std::find(sealed.begin(), sealed.end(), d) == sealed.end()) {
            sealed.push_back(d);
            emitOp(OP_STR_SEAL, 0);
            emitU16((uint16_t)fs->slotOf[d]);
        }
    }
}

// for x in xs (the sequence is on the stack): a list is walked by position in a
// hidden slot, without an iterator object; anything else gets its iterator.
void Compiler::compileForSeq(For* f) {
    emitOp(OP_SEQ_INIT, +1);
    size_t slot = fs->depth - 2;
    size_t loopDepth = fs->depth;
    prepareStrBuilders({f->body.get()});
    size_t start = chunk().code.size();
    emitOp(OP_FOR_SEQ, +1);
    emitU16((uint16_t)slot);
    size_t exitJ = chunk().code.size();
    emitU32(0);
    fs->loops.push_back({start, loopDepth, {}, fs->exits.size()});
    beginScope();
    fs->scopeStarts.back() = loopDepth;
    addLocal(f->vars[0].decl);
    compileBlock(f->body.get(), false);
    endScope(false);
    emitLoop(start);
    fs->depth = loopDepth;
    patchJump(exitJ);
    for (size_t j : fs->loops.back().breakJumps) patchJump(j);
    fs->loops.pop_back();
    emitOp(OP_ENDSCOPE, -2);
    emitU16(2);
}

// for x in a..b [by s]: a counter in three hidden slots, no range object.
void Compiler::compileForRange(For* f, Range* r) {
    compileExpr(r->start.get());
    compileExpr(r->end.get());
    uint8_t flags = r->inclusive ? 1 : 0;
    if (r->step) {
        compileExpr(r->step.get());
    } else {
        emitConst(Value::integer(1));
        flags |= 2;
    }
    emitOp(OP_RANGE_INIT, 0);
    size_t slot = fs->depth - 3;
    size_t loopDepth = fs->depth;
    prepareStrBuilders({f->body.get()});
    size_t start = chunk().code.size();
    emitOp(OP_FOR_RANGE, +1);
    emitU16((uint16_t)slot);
    emitByte(flags);
    size_t exitJ = chunk().code.size();
    emitU32(0);
    fs->loops.push_back({start, loopDepth, {}, fs->exits.size()});
    beginScope();
    fs->scopeStarts.back() = loopDepth;
    addLocal(f->vars[0].decl);
    compileBlock(f->body.get(), false);
    endScope(false);
    emitLoop(start);
    fs->depth = loopDepth;
    patchJump(exitJ);
    for (size_t j : fs->loops.back().breakJumps) patchJump(j);
    fs->loops.pop_back();
    emitOp(OP_ENDSCOPE, -3);
    emitU16(3);
}

void Compiler::compileForRecv(ForRecv* f) {
    size_t base = fs->depth;
    for (auto& c : f->chans) compileExpr(c.get());
    size_t chansDepth = fs->depth;
    size_t start = chunk().code.size();
    std::vector<std::pair<size_t, size_t>> exits;  // (jump operand, received count)
    beginScope();
    for (size_t i = 0; i < f->chans.size(); i++) {
        emitOp(OP_GET_LOCAL, +1);
        emitU16((uint16_t)(base + i));
        emitOp(OP_RECV_ITER, 0);
        exits.push_back({emitJump(OP_JUMP_IF_DONE, 0), i});
        addLocal(f->vars[i].decl);
    }
    fs->loops.push_back({start, chansDepth, {}, fs->exits.size()});
    compileBlock(f->body.get(), false);
    endScope(false);
    emitLoop(start);
    std::vector<size_t> toCommon;
    for (auto& [j, received] : exits) {
        fs->depth = chansDepth + received;
        patchJump(j);
        if (received > 0) {
            emitOp(OP_ENDSCOPE, -(int)received);
            emitU16((uint16_t)received);
        }
        toCommon.push_back(emitJump(OP_JUMP, 0));
    }
    fs->depth = chansDepth;
    for (size_t j : toCommon) patchJump(j);
    for (size_t j : fs->loops.back().breakJumps) patchJump(j);
    fs->loops.pop_back();
    emitOp(OP_ENDSCOPE, -(int)f->chans.size());
    emitU16((uint16_t)f->chans.size());
}

// ------------------------------------------------------------------ expressions

void Compiler::compileIf(If* x, bool wantValue) {
    line = x->pos.line;
    size_t elseJ = compileCondJump(x->cond.get());
    size_t d = fs->depth;
    compileBlock(x->then.get(), wantValue);
    size_t endJ = emitJump(OP_JUMP, 0);
    fs->depth = d;
    patchJump(elseJ);
    if (x->otherwise) {
        Block* ob = x->otherwise.get();
        // elif chains are a block holding a single nested If
        if (ob->stmts.size() == 1 && ob->stmts[0]->kind == NK::ExprStmt &&
            static_cast<ExprStmt*>(ob->stmts[0].get())->expr->kind == NK::If &&
            static_cast<If*>(static_cast<ExprStmt*>(ob->stmts[0].get())->expr.get())->isElif) {
            compileIf(static_cast<If*>(static_cast<ExprStmt*>(ob->stmts[0].get())->expr.get()), wantValue);
        } else {
            compileBlock(ob, wantValue);
        }
    } else if (wantValue) {
        emitOp(OP_NIL, +1);
    }
    patchJump(endJ);
}

static void collectBindings(Pattern* p, std::vector<Decl*>& out) {
    if (!p) return;
    if ((p->k == Pattern::Bind || p->k == Pattern::As) && p->decl) out.push_back(p->decl);
    if (p->restDecl) out.push_back(p->restDecl);
    for (auto& s : p->subs) collectBindings(s.get(), out);
}

static int typeTagFor(const std::string& n) {
    static const std::unordered_map<std::string, int> tags = {
        {"nil", (int)Tag::Nil},       {"bool", (int)Tag::Bool},   {"int", (int)Tag::Int},
        {"float", (int)Tag::Float},   {"str", (int)Tag::Str},     {"list", (int)Tag::List},
        {"tuple", (int)Tag::Tuple},   {"map", (int)Tag::Map},     {"set", (int)Tag::Set},
        {"range", (int)Tag::Range},   {"fn", (int)Tag::Fn},       {"flow", (int)Tag::Flow},
        {"task", (int)Tag::Task},     {"vec", (int)Tag::Vec},     {"mat", (int)Tag::Mat},
        {"quat", (int)Tag::Quat},     {"rng", (int)Tag::Rng},     {"time", (int)Tag::Time},
        {"num", 0xFF},                {"Error", 0xFE},            {"module", (int)Tag::Module},
    };
    auto it = tags.find(n);
    return it == tags.end() ? -1 : it->second;
}

void Compiler::compilePattern(Pattern* p, const std::function<void()>& load, std::vector<size_t>& fails) {
    auto slotOf = [&](Decl* d) -> uint16_t { return (uint16_t)fs->slotOf[d]; };
    switch (p->k) {
        case Pattern::Wildcard: return;
        case Pattern::Bind:
            load();
            emitOp(OP_SET_LOCAL, -1);
            emitU16(slotOf(p->decl));
            return;
        case Pattern::Literal:
            load();
            compileExpr(p->value.get());
            emitOp(OP_EQ, -1);
            fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
            return;
        case Pattern::Value:
            load();
            compileExpr(p->value.get());
            emitOp(OP_MATCH_VALUE, -1);
            fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
            return;
        case Pattern::RangeP: {
            bool isStr = p->value->kind == NK::StrLit;
            load();
            emitOp(OP_TEST_TYPE, 0);
            emitByte(isStr ? (uint8_t)Tag::Str : 0xFF);
            fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
            load();
            compileExpr(p->value.get());
            emitOp(OP_GE, -1);
            fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
            load();
            compileExpr(p->value2.get());
            emitOp(p->inclusive ? OP_LE : OP_LT, -1);
            fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
            return;
        }
        case Pattern::TypeP_: {
            if (p->name == "any") return;
            int tag = typeTagFor(p->name);
            if (tag < 0) {
                error(p->pos, "unknown type '" + p->name + "' in pattern");
                return;
            }
            load();
            emitOp(OP_TEST_TYPE, 0);
            emitByte((uint8_t)tag);
            fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
            return;
        }
        case Pattern::ClassP: {
            load();
            compileExpr(p->value.get());
            emitOp(OP_INSTANCE_OF, -1);
            fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
            for (size_t i = 0; i < p->subs.size(); i++) {
                auto sub = [&, i]() {
                    load();
                    emitOp(OP_POS_FIELD, 0);
                    emitU16((uint16_t)i);
                };
                compilePattern(p->subs[i].get(), sub, fails);
            }
            return;
        }
        case Pattern::MapP: {
            load();
            emitOp(OP_TEST_KEY, 0);
            emitU16(0xFFFF);  // is it a map at all?
            fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
            for (size_t i = 0; i < p->keys.size(); i++) {
                Value key;
                Expr* k = p->keys[i].get();
                if (k->kind == NK::StrLit) key = vm.vstr(static_cast<StrLit*>(k)->value);
                else if (k->kind == NK::IntLit) key = Value::integer(static_cast<IntLit*>(k)->value);
                else if (k->kind == NK::FloatLit) key = Value::number(static_cast<FloatLit*>(k)->value);
                else if (k->kind == NK::BoolLit) key = Value::boolean(static_cast<BoolLit*>(k)->value);
                else if (k->kind == NK::Unary && static_cast<Unary*>(k)->operand->kind == NK::IntLit)
                    key = Value::integer(-static_cast<IntLit*>(static_cast<Unary*>(k)->operand.get())->value);
                else if (k->kind == NK::Unary && static_cast<Unary*>(k)->operand->kind == NK::FloatLit)
                    key = Value::number(-static_cast<FloatLit*>(static_cast<Unary*>(k)->operand.get())->value);
                uint16_t kc = constant(key);
                load();
                emitOp(OP_TEST_KEY, 0);
                emitU16(kc);
                fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
                auto sub = [&, kc]() {
                    load();
                    emitOp(OP_KEY, 0);
                    emitU16(kc);
                };
                compilePattern(p->subs[i].get(), sub, fails);
            }
            return;
        }
        case Pattern::TupleP:
        case Pattern::ListP: {
            bool isList = p->k == Pattern::ListP;
            bool hasRest = isList && p->restIndex >= 0;
            load();
            emitOp(OP_TEST_LEN, 0);
            emitByte(hasRest ? 1 : 0);
            emitByte(isList ? 0 : 1);
            emitU16((uint16_t)p->subs.size());
            fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
            int n = (int)p->subs.size();
            int before = hasRest ? p->restIndex : n;
            for (int i = 0; i < n; i++) {
                int16_t idx = i < before ? (int16_t)i : (int16_t)(i - n);  // negative: from the end
                auto sub = [&, idx]() {
                    load();
                    emitOp(OP_ELEM, 0);
                    emitU16((uint16_t)idx);
                };
                compilePattern(p->subs[i].get(), sub, fails);
            }
            if (hasRest && p->restDecl) {
                load();
                emitOp(OP_SLICE_FROM, 0);
                emitU16((uint16_t)p->restIndex);
                emitU16((uint16_t)(n - p->restIndex));
                emitOp(OP_SET_LOCAL, -1);
                emitU16(slotOf(p->restDecl));
            }
            return;
        }
        case Pattern::Alt: {
            std::vector<size_t> oks;
            for (size_t i = 0; i < p->subs.size(); i++) {
                if (i + 1 == p->subs.size()) {
                    compilePattern(p->subs[i].get(), load, fails);
                } else {
                    std::vector<size_t> subFails;
                    compilePattern(p->subs[i].get(), load, subFails);
                    oks.push_back(emitJump(OP_JUMP, 0));
                    for (size_t j : subFails) patchJump(j);
                }
            }
            for (size_t j : oks) patchJump(j);
            return;
        }
        case Pattern::As:
            compilePattern(p->subs[0].get(), load, fails);
            load();
            emitOp(OP_SET_LOCAL, -1);
            emitU16(slotOf(p->decl));
            return;
    }
}

void Compiler::compileMatch(Match* m, bool wantValue) {
    line = m->pos.line;
    compileExpr(m->subject.get());
    size_t subjSlot = fs->depth - 1;
    beginScope();
    fs->scopeStarts.back() = subjSlot;  // the subject is dropped at the end
    std::vector<size_t> ends;
    size_t armDepth = fs->depth;
    auto load = [&]() {
        emitOp(OP_GET_LOCAL, +1);
        emitU16((uint16_t)subjSlot);
    };
    for (auto& arm : m->arms) {
        line = arm.pat->pos.line;
        fs->depth = armDepth;
        std::vector<Decl*> binds;
        collectBindings(arm.pat.get(), binds);
        beginScope();
        for (Decl* d : binds) {
            emitOp(OP_NIL, +1);
            addLocal(d);
        }
        size_t bindDepth = fs->depth;
        std::vector<size_t> fails;
        compilePattern(arm.pat.get(), load, fails);
        if (arm.guard) {
            compileExpr(arm.guard.get());
            fails.push_back(emitJump(OP_JUMP_IF_FALSE, -1));
        }
        compileBlock(arm.body.get(), wantValue);
        endScope(wantValue);
        ends.push_back(emitJump(OP_JUMP, 0));
        fs->depth = bindDepth;
        for (size_t j : fails) patchJump(j);
        if (!binds.empty()) {
            emitOp(OP_ENDSCOPE, -(int)binds.size());
            emitU16((uint16_t)binds.size());
        }
    }
    fs->depth = armDepth;
    if (wantValue) emitOp(OP_NIL, +1);
    for (size_t j : ends) patchJump(j);
    endScope(wantValue);
}

void Compiler::compileCatch(Catch* c) {
    size_t base = fs->depth;
    size_t attemptSlot = 0;
    if (c->hasRetry) {
        emitConst(Value::integer(0));
        attemptSlot = fs->depth - 1;
    }
    size_t startDepth = fs->depth;
    size_t loopStart = chunk().code.size();
    compileExpr(c->expr.get());
    size_t okJ = emitJump(OP_JUMP_IF_NOT_ERROR, 0);
    beginScope();
    fs->scopeStarts.back() = startDepth;
    addLocal(c->errDecl);
    if (c->hasRetry) fs->retries.push_back({loopStart, startDepth, attemptSlot, fs->exits.size()});
    compileBlock(c->handler.get(), true);
    if (c->hasRetry) fs->retries.pop_back();
    endScope(true);
    patchJump(okJ);
    if (c->hasRetry) {
        emitOp(OP_ENDSCOPE_KEEP, -1);
        emitU16(1);
    }
    (void)base;
}

// Can a lambda's body be compiled straight into a loop? Not if it returns from
// the lambda, propagates errors, or makes closures (they'd capture its frame).
static bool inlinableBody(Node* n) {
    if (!n) return true;
    switch (n->kind) {
        case NK::Return: case NK::Try: case NK::Lambda: case NK::FnDecl: case NK::ClassDecl: case NK::Fork:
        case NK::Defer: case NK::Retry: case NK::Select: case NK::With: case NK::Region: case NK::InterfaceDecl:
        case NK::EnumDecl: case NK::Use:
            return false;
        default: break;
    }
    bool ok = true;
    forEachChild(n, [&](Node* c) {
        if (ok && !inlinableBody(c)) ok = false;
    });
    return ok;
}

// self.x inside a method of the class that has field x: a fixed slot
bool Compiler::m_selfField(Member* m) {
    return m->object->kind == NK::Ident && static_cast<Ident*>(m->object.get())->name == "self" && fs->cls &&
           fs->cls->fieldIndex.count(m->name);
}

// xs.map(_ * 2), xs.keep(...), xs.fold(0, _a + _b), ...: when xs is a list,
// range, tuple, string or set, the lambda's body runs inside a loop right here
// (no closure, no call per item). Anything else takes the normal method call.
bool Compiler::tryInlineHof(Member* m) {
    enum { Map, Keep, Skip, Each, Fold, Any, All, Count, FindBy, SumBy };
    static const std::unordered_map<std::string, int> kinds = {
        {"map", Map}, {"keep", Keep}, {"skip", Skip}, {"each", Each}, {"fold", Fold}, {"any", Any},
        {"all", All}, {"count", Count}, {"find_by", FindBy}, {"sum_by", SumBy}};
    auto kit = kinds.find(m->name);
    if (kit == kinds.end() || hasNamed(m->args)) return false;
    int kind = kit->second;
    if (m->args.size() != (kind == Fold ? 2u : 1u)) return false;
    if (m->args.back()->kind != NK::Lambda) return false;
    FnDecl* lf = static_cast<Lambda*>(m->args.back().get())->fn.get();
    if (lf->params.size() > (kind == Fold ? 2u : 1u) || (kind == Fold && lf->params.size() != 2)) return false;
    for (auto& p : lf->params)
        if (p.def || p.type || !p.decl) return false;
    if (lf->ret || lf->isFork || lf->isPar || !lf->body || !lf->info || !inlinableBody(lf->body.get())) return false;

    compileExpr(m->object.get());  // [r]
    size_t d0 = fs->depth;
    size_t genericJ = emitJump(OP_JUMP_IF_NOT_SEQ, 0);
    if (kind == Each) emitOp(OP_DUP, +1);  // each gives back the collection
    emitOp(OP_SEQ_INIT, +1);  // a list walked by position (no iterator object), else its iterator
    size_t itSlot = fs->depth - 2;
    switch (kind) {
        case Map: case Keep: case Skip:
            emitOp(OP_LIST, +1);
            emitU16(0);
            break;
        case Fold: compileExpr(m->args[0].get()); break;
        case Any: emitOp(OP_FALSE, +1); break;
        case All: emitOp(OP_TRUE, +1); break;
        case Count: case SumBy: emitConst(Value::integer(0)); break;
        default: emitOp(OP_NIL, +1); break;  // each (unused), find_by (the result)
    }
    size_t accSlot = fs->depth - 1;
    size_t loopStart = chunk().code.size();
    emitOp(OP_FOR_SEQ, +1);
    emitU16((uint16_t)itSlot);
    size_t exitJ = chunk().code.size();
    emitU32(0);
    size_t xSlot = fs->depth - 1;
    beginScope();
    fs->scopeStarts.back() = xSlot;  // the item and the lambda's locals end with each round
    if (kind == Fold) {
        bindSlot(*fs, lf->params[0].decl, accSlot);
        bindSlot(*fs, lf->params[1].decl, xSlot);
    } else if (!lf->params.empty()) {
        bindSlot(*fs, lf->params[0].decl, xSlot);
    }
    for (Decl* h : lf->info->hoisted) {
        emitOp(OP_UNDEF, +1);
        bindSlot(*fs, h, fs->depth - 1);
    }
    compileBlock(lf->body.get(), true);  // [.., x, locals, v]
    std::vector<size_t> early;
    auto leaveRound = [&]() {  // pop this round's slots without changing the compile-time depth
        size_t n = fs->depth - xSlot;
        emitOp(OP_ENDSCOPE, 0);
        emitU16((uint16_t)n);
        early.push_back(emitJump(OP_JUMP, 0));
    };
    auto getLocal = [&](size_t slot) {
        emitOp(OP_GET_LOCAL, +1);
        emitU16((uint16_t)slot);
    };
    auto setLocal = [&](size_t slot) {
        emitOp(OP_SET_LOCAL, -1);
        emitU16((uint16_t)slot);
    };
    switch (kind) {
        case Map:
            emitOp(OP_LIST_APPEND, -1);
            emitU16((uint16_t)accSlot);
            break;
        case Keep: case Skip: {
            if (kind == Skip) emitOp(OP_NOT, 0);
            size_t j = emitJump(OP_JUMP_IF_FALSE, -1);
            getLocal(xSlot);
            emitOp(OP_LIST_APPEND, -1);
            emitU16((uint16_t)accSlot);
            patchJump(j);
            break;
        }
        case Each: emitOp(OP_POP, -1); break;
        case Fold: setLocal(accSlot); break;
        case Any: {
            size_t j = emitJump(OP_JUMP_IF_FALSE, -1);
            emitOp(OP_TRUE, +1);
            setLocal(accSlot);
            leaveRound();
            patchJump(j);
            break;
        }
        case All: {
            size_t j = emitJump(OP_JUMP_IF_FALSE, -1);
            size_t cont = emitJump(OP_JUMP, 0);
            patchJump(j);
            emitOp(OP_FALSE, +1);
            setLocal(accSlot);
            leaveRound();
            patchJump(cont);
            break;
        }
        case Count: {
            size_t j = emitJump(OP_JUMP_IF_FALSE, -1);
            emitOp(OP_INC_LOCAL, 0);
            emitU16((uint16_t)accSlot);
            emitByte(1);
            patchJump(j);
            break;
        }
        case SumBy:
            getLocal(accSlot);
            emitOp(OP_SWAP, 0);
            emitOp(OP_ADD, -1);
            setLocal(accSlot);
            break;
        case FindBy: {
            size_t j = emitJump(OP_JUMP_IF_FALSE, -1);
            getLocal(xSlot);
            setLocal(accSlot);
            leaveRound();
            patchJump(j);
            break;
        }
    }
    endScope(false);
    emitLoop(loopStart);
    patchJump(exitJ);
    for (size_t j : early) patchJump(j);
    // [r?, seq, position, acc] -> [result]
    if (kind == Each) {
        emitOp(OP_ENDSCOPE, -3);
        emitU16(3);
    } else {
        emitOp(OP_ENDSCOPE_KEEP, -2);
        emitU16(2);
    }
    size_t endJ = emitJump(OP_JUMP, 0);
    for (auto& p : lf->params) fs->slotOf.erase(p.decl);
    for (Decl* h : lf->info->hoisted) fs->slotOf.erase(h);
    // anything else: the normal method call
    fs->depth = d0;
    patchJump(genericJ);
    uint8_t k;
    uint16_t idx;
    fallbackFor(m->ufcs, k, idx);
    for (auto& a : m->args) compileExpr(a.get());
    emitOp(OP_INVOKE, -(int)m->args.size());
    emitU16((uint16_t)vm.sym(m->name));
    emitByte((uint8_t)m->args.size());
    emitByte(k);
    emitU16(idx);
    emitIC();
    patchJump(endJ);
    return true;
}

void Compiler::compileMember(Member* m, bool raw) {
    if (m->isCall && !raw && tryInlineHof(m)) return;
    uint8_t k;
    uint16_t idx;
    fallbackFor(m->ufcs, k, idx);
    if (m->isCall && hasNamed(m->args)) {
        // obj.method(a, name: b): take the method as a value, then call it by names
        compileExpr(m->object.get());
        emitOp(OP_GET_MEMBER_RAW, 0);
        emitU16((uint16_t)vm.sym(m->name));
        emitByte(k);
        emitU16(idx);
        emitIC();
        compileArgsNamed(m->args, m->pos);
        return;
    }
    if (m->isCall) {
        compileExpr(m->object.get());
        for (auto& a : m->args) compileExpr(a.get());
        emitOp(OP_INVOKE, -(int)m->args.size());
        emitU16((uint16_t)vm.sym(m->name));
        emitByte((uint8_t)m->args.size());
        emitByte(k);
        emitU16(idx);
        emitIC();
        return;
    }
    bool selfField = !raw && m->object->kind == NK::Ident && static_cast<Ident*>(m->object.get())->name == "self" &&
                     fs->cls && fs->cls->fieldIndex.count(m->name);
    compileExpr(m->object.get());
    if (selfField) {
        emitOp(OP_GET_FIELD, 0);
        emitU16((uint16_t)fs->cls->fieldIndex[m->name]);
        return;
    }
    emitOp(raw ? OP_GET_MEMBER_RAW : OP_GET_MEMBER, 0);
    emitU16((uint16_t)vm.sym(m->name));
    emitByte(k);
    emitU16(idx);
    emitIC();
}

// select: SELECT leaves a tuple (arm index, value); each arm checks the index.
void Compiler::compileSelect(Select* s) {
    beginScope();
    int nRecv = 0;
    SelectArm* after = nullptr;
    for (auto& a : s->arms) {
        if (a.kind == SelectArm::Recv) {
            compileExpr(a.expr.get());
            nRecv++;
        } else if (a.kind == SelectArm::After) {
            after = &a;
        }
    }
    uint8_t flags = 0;
    if (after) {
        compileExpr(after->expr.get());
        flags |= 1;
    }
    for (auto& a : s->arms)
        if (a.kind == SelectArm::Else) flags |= 2;
    emitOp(OP_SELECT, 1 - nRecv - (after ? 1 : 0));
    emitByte((uint8_t)nRecv);
    emitByte(flags);
    size_t resultSlot = fs->depth - 1;
    std::vector<size_t> ends;
    int next = 0;
    for (auto& a : s->arms) {
        int armIndex = a.kind == SelectArm::Recv ? next++ : a.kind == SelectArm::After ? nRecv : -1;
        line = a.pos.line;
        emitOp(OP_GET_LOCAL, +1);
        emitU16((uint16_t)resultSlot);
        emitOp(OP_ELEM, 0);
        emitU16(0);
        emitConst(Value::integer(armIndex));
        emitOp(OP_EQ, -1);
        size_t skip = emitJump(OP_JUMP_IF_FALSE, -1);
        beginScope();
        if (a.kind == SelectArm::Recv && a.decl) {
            emitOp(OP_GET_LOCAL, +1);
            emitU16((uint16_t)resultSlot);
            emitOp(OP_ELEM, 0);
            emitU16(1);
            addLocal(a.decl);
        }
        compileBlock(a.body.get(), false);
        endScope(false);
        ends.push_back(emitJump(OP_JUMP, 0));
        patchJump(skip);
    }
    for (size_t e : ends) patchJump(e);
    endScope(false);
}

bool Compiler::hasNamed(const std::vector<ExprP>& args) {
    for (auto& a : args)
        if (a && a->kind == NK::NamedArg) return true;
    return false;
}

// Pushes the arguments (named ones last) and emits CALL_NAMED with their names.
void Compiler::compileArgsNamed(const std::vector<ExprP>& args, SrcPos pos) {
    std::vector<uint16_t> names;
    for (auto& a : args) {
        compileExpr(a.get());
        if (a->kind == NK::NamedArg) names.push_back(strConst(static_cast<NamedArg*>(a.get())->name));
    }
    if (args.size() > 255) error(pos, "too many arguments (max 255)");
    emitOp(OP_CALL_NAMED, -(int)args.size());
    emitByte((uint8_t)args.size());
    emitByte((uint8_t)names.size());
    for (uint16_t n : names) emitU16(n);
}

void Compiler::compileFork(Fork* f) {
    Expr* c = f->call.get();
    if ((c->kind == NK::Call && hasNamed(static_cast<Call*>(c)->args)) ||
        (c->kind == NK::Member && hasNamed(static_cast<Member*>(c)->args)))
        error(c->pos, "named arguments can't be used with fork or par yet; pass them by position");
    Op forkOp = f->parallel ? OP_PAR : OP_FORK;
    if (c->kind == NK::Call) {
        auto* call = static_cast<Call*>(c);
        compileExpr(call->callee.get());
        for (auto& a : call->args) compileExpr(a.get());
        emitOp(forkOp, -(int)call->args.size());
        emitByte((uint8_t)call->args.size());
        return;
    }
    if (c->kind == NK::Member) {
        auto* m = static_cast<Member*>(c);
        bool wasCall = m->isCall;
        m->isCall = false;
        compileMember(m, true);
        m->isCall = wasCall;
        for (auto& a : m->args) compileExpr(a.get());
        emitOp(forkOp, -(int)m->args.size());
        emitByte((uint8_t)m->args.size());
        return;
    }
    compileExpr(c);
    emitOp(forkOp, 0);
    emitByte(0);
}

void Compiler::compileExpr(Expr* e) {
    if (!e) {
        emitOp(OP_NIL, +1);
        return;
    }
    int savedLine = line;
    if (e->pos.line > 0) line = e->pos.line;
    switch (e->kind) {
        case NK::IntLit: emitConst(Value::integer(static_cast<IntLit*>(e)->value)); break;
        case NK::FloatLit: emitConst(Value::number(static_cast<FloatLit*>(e)->value)); break;
        case NK::StrLit: emitConst(vm.vstr(static_cast<StrLit*>(e)->value)); break;
        case NK::BoolLit: emitOp(static_cast<BoolLit*>(e)->value ? OP_TRUE : OP_FALSE, +1); break;
        case NK::NilLit: emitOp(OP_NIL, +1); break;
        case NK::FStr: {
            auto* fsx = static_cast<FStr*>(e);
            if (fsx->parts.size() > 1) {
                // one string grows as each part is evaluated (in order), instead of a string per part
                emitOp(OP_FSTR_BEGIN, +1);
                for (auto& part : fsx->parts) {
                    if (part.expr) {
                        compileExpr(part.expr.get());
                        emitOp(OP_FSTR_ADD, -1);
                        emitU16(part.spec.empty() ? 0xFFFF : strConst(part.spec));
                    } else {
                        emitOp(OP_FSTR_LIT, 0);
                        emitU16(constant(vm.vstr(part.text)));
                    }
                }
                break;
            }
            int n = 0;
            for (auto& part : fsx->parts) {
                if (part.expr) {
                    compileExpr(part.expr.get());
                    emitOp(OP_FORMAT, 0);
                    emitU16(part.spec.empty() ? 0xFFFF : strConst(part.spec));
                } else {
                    emitConst(vm.vstr(part.text));
                }
                n++;
            }
            if (n == 0) {
                emitConst(vm.vstr(""));
            } else if (n > 1) {
                emitOp(OP_CONCAT, 1 - n);
                emitU16((uint16_t)n);
            } else if (!fsx->parts[0].expr) {
                // plain text only
            }
            break;
        }
        case NK::Ident: {
            auto* id = static_cast<Ident*>(e);
            if (id->selfMember) {  // a field or method by its bare name: self.name
                auto* sm = static_cast<Member*>(id->selfMember.get());
                compileMember(sm, sm->ref);
                break;
            }
            emitGet(id->decl, id->pos);
            // fn tick / fn version = "1": the name alone calls it
            if (!id->callee && !id->ref && fnAutoCalls(fnDeclOf(id->decl))) {
                emitOp(OP_CALL, 0);
                emitByte(0);
            }
            break;
        }
        case NK::Unary: {
            auto* u = static_cast<Unary*>(e);
            compileExpr(u->operand.get());
            emitOp(u->op == UnOp::Neg ? OP_NEG : u->op == UnOp::Not ? OP_NOT : OP_BNOT, 0);
            break;
        }
        case NK::Binary: {
            auto* b = static_cast<Binary*>(e);
            if (b->op == BinOp::And || b->op == BinOp::Or) {
                compileExpr(b->left.get());
                size_t j = emitJump(b->op == BinOp::And ? OP_JUMP_IF_FALSE_KEEP : OP_JUMP_IF_TRUE_KEEP, -1);
                compileExpr(b->right.get());
                patchJump(j);
                break;
            }
            compileExpr(b->left.get());
            compileExpr(b->right.get());
            Op op;
            switch (b->op) {
                case BinOp::Add: op = OP_ADD; break;
                case BinOp::Sub: op = OP_SUB; break;
                case BinOp::Mul: op = OP_MUL; break;
                case BinOp::Div: op = OP_DIV; break;
                case BinOp::Mod: op = OP_MOD; break;
                case BinOp::Pow: op = OP_POW; break;
                case BinOp::Root: op = OP_ROOT; break;
                case BinOp::IntDiv: op = OP_IDIV; break;
                case BinOp::Eq: op = OP_EQ; break;
                case BinOp::Ne: op = OP_NE; break;
                case BinOp::Lt: op = OP_LT; break;
                case BinOp::Le: op = OP_LE; break;
                case BinOp::Gt: op = OP_GT; break;
                case BinOp::Ge: op = OP_GE; break;
                case BinOp::Is: op = OP_IS; break;
                case BinOp::IsNot: op = OP_ISNOT; break;
                case BinOp::In: op = OP_IN; break;
                case BinOp::NotIn: op = OP_NOTIN; break;
                case BinOp::BitAnd: op = OP_BAND; break;
                case BinOp::BitOr: op = OP_BOR; break;
                case BinOp::BitXor: op = OP_BXOR; break;
                case BinOp::Shl: op = OP_SHL; break;
                case BinOp::Shr: op = OP_SHR; break;
                case BinOp::Send: op = OP_SEND; break;
                default: op = OP_ADD; break;
            }
            emitOp(op, -1);
            break;
        }
        case NK::Range: {
            auto* r = static_cast<Range*>(e);
            compileExpr(r->start.get());
            compileExpr(r->end.get());
            uint8_t flags = r->inclusive ? 1 : 0;
            if (r->step) {
                compileExpr(r->step.get());
                flags |= 2;
            }
            emitOp(OP_RANGE, (flags & 2) ? -2 : -1);
            emitByte(flags);
            break;
        }
        case NK::Call: {
            auto* c = static_cast<Call*>(e);
            if (c->callee->kind == NK::Ident && static_cast<Ident*>(c->callee.get())->selfMember) {
                // describe(x) in a method: self.describe(x)
                auto* sm = static_cast<Member*>(static_cast<Ident*>(c->callee.get())->selfMember.get());
                sm->isCall = true;
                std::swap(sm->args, c->args);
                compileMember(sm, false);
                std::swap(sm->args, c->args);
                sm->isCall = false;
                break;
            }
            compileExpr(c->callee.get());
            if (hasNamed(c->args)) {
                compileArgsNamed(c->args, c->pos);
                break;
            }
            for (auto& a : c->args) compileExpr(a.get());
            if (c->args.size() > 255) error(c->pos, "too many arguments (max 255)");
            emitOp(OP_CALL, -(int)c->args.size());
            emitByte((uint8_t)c->args.size());
            break;
        }
        case NK::Member: compileMember(static_cast<Member*>(e), static_cast<Member*>(e)->ref); break;
        case NK::NamedArg: compileExpr(static_cast<NamedArg*>(e)->value.get()); break;
        case NK::Index: {
            auto* ix = static_cast<Index*>(e);
            compileExpr(ix->object.get());
            compileExpr(ix->index.get());
            emitOp(OP_GET_INDEX, -1);
            break;
        }
        case NK::ListLit: {
            auto* l = static_cast<ListLit*>(e);
            for (auto& x : l->elems) compileExpr(x.get());
            emitOp(OP_LIST, 1 - (int)l->elems.size());
            emitU16((uint16_t)l->elems.size());
            break;
        }
        case NK::TupleLit: {
            auto* l = static_cast<TupleLit*>(e);
            for (auto& x : l->elems) compileExpr(x.get());
            emitOp(OP_TUPLE, 1 - (int)l->elems.size());
            emitU16((uint16_t)l->elems.size());
            break;
        }
        case NK::SetLit: {
            auto* l = static_cast<SetLit*>(e);
            for (auto& x : l->elems) compileExpr(x.get());
            emitOp(OP_SET, 1 - (int)l->elems.size());
            emitU16((uint16_t)l->elems.size());
            break;
        }
        case NK::MapLit: {
            auto* m = static_cast<MapLit*>(e);
            for (size_t i = 0; i < m->keys.size(); i++) {
                compileExpr(m->keys[i].get());
                compileExpr(m->values[i].get());
            }
            emitOp(OP_MAP, 1 - 2 * (int)m->keys.size());
            emitU16((uint16_t)m->keys.size());
            break;
        }
        case NK::Lambda: {
            auto* l = static_cast<Lambda*>(e);
            compileFunction(l->fn.get(), l->fn->name == "<task>" ? "<task>" : "<lambda>");
            break;
        }
        case NK::If: compileIf(static_cast<If*>(e), true); break;
        case NK::Match: compileMatch(static_cast<Match*>(e), true); break;
        case NK::Catch: compileCatch(static_cast<Catch*>(e)); break;
        case NK::Try:
            compileExpr(static_cast<Try*>(e)->expr.get());
            if (fs->exits.empty()) {
                emitOp(OP_TRY, 0);
            } else {
                size_t ok = emitJump(OP_JUMP_IF_NOT_ERROR, 0);
                size_t saved = fs->depth;
                emitExits(0);
                emitOp(OP_RETURN, -1);
                fs->depth = saved;
                patchJump(ok);
            }
            break;
        case NK::Fork: compileFork(static_cast<Fork*>(e)); break;
        default: error(e->pos, "internal: can't compile expression"); emitOp(OP_NIL, +1); break;
    }
    line = savedLine;
}

}  // namespace ixo
