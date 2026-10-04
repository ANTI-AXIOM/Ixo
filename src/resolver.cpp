#include "resolver.hpp"

#include <algorithm>

namespace ixo {

Resolver::Resolver(ModuleUnit& mod, Diagnostics& diags, ModuleHost& host) : mod(mod), diags(diags), host(host) {}

// ------------------------------------------------------------------ lookup

Decl* Resolver::lookupLocalScopes(const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto f = it->names.find(name);
        if (f != it->names.end()) return f->second;
    }
    return nullptr;
}

Decl* Resolver::lookup(const std::string& name, bool markCapture) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto f = it->names.find(name);
        if (f != it->names.end()) {
            if (markCapture && it->fn != curFn) f->second->captured = true;
            return f->second;
        }
    }
    auto g = mod.globals.find(name);
    if (g != mod.globals.end()) return g->second;
    auto b = mod.builtinDecls.find(name);
    if (b != mod.builtinDecls.end()) return b->second;
    int idx = host.builtinIndex(name);
    if (idx >= 0) {
        Decl* d = mod.newDecl(name, DeclKind::Builtin, {});
        d->builtinIndex = idx;
        d->isGlobal = true;
        mod.builtinDecls[name] = d;
        return d;
    }
    return nullptr;
}

bool Resolver::selfMemberName(const std::string& n) {
    if (n.empty() || n[0] == '_' || n == "self") return false;
    if (curClass) return curClass->fieldIndex.count(n) || curClass->methods.count(n);
    if (curIface) return curIface->methods.count(n) > 0;
    if (curEnum)
        for (auto& m : curEnum->methods)
            if (m->name == n) return true;
    return false;
}

// hp in a method of a class with a field hp: self.hp (locals and parameters
// come first; a field hides a module variable of the same name)
bool Resolver::bindSelfMember(Ident* id, bool assigning) {
    if (!selfMemberName(id->name) || lookupLocalScopes(id->name)) return false;
    if (!lookup("self", false)) return false;  // not inside a method
    // a method doesn't hide a built-in (json, print): write self.json for the method
    bool isField = curClass && curClass->fieldIndex.count(id->name);
    if (!isField && host.builtinIndex(id->name) >= 0) return false;
    if (assigning && curClass && !curClass->fieldIndex.count(id->name)) {
        error(id->pos, "'" + id->name + "' is a method; it can't be assigned", (int)id->name.size());
        return true;
    }
    auto g = mod.globals.find(id->name);
    if (g != mod.globals.end() && g->second->kind == DeclKind::Var && !warnedHides[id->name]) {
        warnedHides[id->name] = true;
        diags.warning(mod.path, id->pos,
                      "the field '" + id->name + "' hides the module variable '" + id->name +
                          "' here; rename one of them",
                      (int)id->name.size());
    }
    auto m = std::make_unique<Member>(id->pos, std::make_unique<Ident>(id->pos, "self"), id->name, id->pos);
    m->ref = id->ref || id->callee;
    id->selfMember = std::move(m);
    resolveExpr(id->selfMember.get());
    return true;
}

Decl* Resolver::declareLocal(const std::string& name, DeclKind k, SrcPos p, bool blockScoped) {
    Decl* d = mod.newDecl(name, k, p);
    d->owner = curFn;
    d->blockScoped = blockScoped;
    if (!scopes.empty() && k != DeclKind::Hidden) scopes.back().names[name] = d;
    return d;
}

static int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string Resolver::suggest(const std::string& name) {
    std::string best;
    int bestD = name.size() <= 3 ? 1 : 2;
    auto consider = [&](const std::string& cand) {
        if (cand.empty() || cand[0] == '$' || cand == name) return;
        int d = editDistance(name, cand);
        if (d <= bestD && (best.empty() || d < editDistance(name, best))) best = cand;
    };
    for (auto& s : scopes)
        for (auto& [n, d] : s.names) consider(n);
    for (auto& [n, d] : mod.globals) consider(n);
    for (auto& n : host.builtinNames()) consider(n);
    return best;
}

// ------------------------------------------------------------------ module

void Resolver::collectAssigned(Node* n, std::vector<std::pair<std::string, SrcPos>>& out) {
    if (!n) return;
    switch (n->kind) {
        case NK::Assign: {
            auto* a = static_cast<Assign*>(n);
            if (a->op == BinOp::None && a->target->kind == NK::Ident) {
                auto* id = static_cast<Ident*>(a->target.get());
                if (!id->isPlaceholder() && id->name != "self") out.push_back({id->name, id->pos});
            } else {
                collectAssigned(a->target.get(), out);
            }
            collectAssigned(a->value.get(), out);
            return;
        }
        case NK::Destructure: {
            auto* d = static_cast<Destructure*>(n);
            for (auto& t : d->targets) {
                if (t->kind == NK::Ident) {
                    auto* id = static_cast<Ident*>(t.get());
                    if (!id->isPlaceholder() && id->name != "self") out.push_back({id->name, id->pos});
                } else {
                    collectAssigned(t.get(), out);
                }
            }
            collectAssigned(d->value.get(), out);
            return;
        }
        case NK::Recv: {
            auto* r = static_cast<Recv*>(n);
            auto* id = static_cast<Ident*>(r->target.get());
            out.push_back({id->name, id->pos});
            collectAssigned(r->chan.get(), out);
            return;
        }
        case NK::FnDecl: {
            auto* f = static_cast<FnDecl*>(n);
            out.push_back({f->name, f->namePos});
            return;
        }
        case NK::ClassDecl: {
            auto* c = static_cast<ClassDecl*>(n);
            out.push_back({c->name, c->namePos});
            return;
        }
        case NK::InterfaceDecl: {
            auto* d = static_cast<InterfaceDecl*>(n);
            out.push_back({d->name, d->namePos});
            return;
        }
        case NK::EnumDecl: {
            auto* d = static_cast<EnumDecl*>(n);
            out.push_back({d->name, d->namePos});
            return;
        }
        case NK::Lambda: return;
        case NK::Region:
            // variables first assigned in @arena / @frame / ... belong to that block
            if (static_cast<Region*>(n)->isArena()) return;
            forEachChild(n, [&](Node* c) { collectAssigned(c, out); });
            return;
        default: forEachChild(n, [&](Node* c) { collectAssigned(c, out); }); return;
    }
}

void Resolver::processUse(Use* u) {
    std::string dotted = u->dotted();
    ModuleUnit* m = host.importModule(dotted, mod.path, diags, u->pos, mod.path);
    if (!m) return;
    u->module = m;
    auto exports = m->exportNames();
    if (!u->selective) {
        std::string bindName = u->alias.empty() ? u->path.back() : u->alias;
        Decl* md = mod.addGlobal(bindName, DeclKind::Module, u->pos);
        if (md->kind == DeclKind::Var) md->kind = DeclKind::Module;
        md->importFrom = m;
        u->moduleDecl = md;
        for (auto& name : exports) {
            Decl* d = mod.addGlobal(name, DeclKind::Import, u->pos);
            if (d->kind == DeclKind::Import || d->kind == DeclKind::Var) {
                d->kind = DeclKind::Import;
                d->importFrom = m;
                d->importName = name;
            }
            u->imported.push_back({d, name});
        }
        return;
    }
    for (auto& item : u->items) {
        bool found = std::find(exports.begin(), exports.end(), item.name) != exports.end();
        if (!found) {
            std::string msg = "module '" + dotted + "' has no exported '" + item.name + "'";
            if (!m->isNative && m->globals.count(item.name))
                msg += " (it exists but is private: mark it with * to export it, e.g. fn " + item.name + "*)";
            error(item.pos, msg, (int)item.name.size());
            continue;
        }
        std::string local = item.alias.empty() ? item.name : item.alias;
        Decl* d = mod.addGlobal(local, DeclKind::Import, item.pos);
        d->kind = DeclKind::Import;
        d->importFrom = m;
        d->importName = item.name;
        item.decl = d;
        u->imported.push_back({d, item.name});
    }
}

void Resolver::prescanModule(Block* program) {
    bool repl = mod.name == "<repl>";
    std::vector<InterfaceDecl*> laterInterfaces;  // built once every name is known (for `is` bases)
    struct BuildInterfaces {
        Resolver& r;
        std::vector<InterfaceDecl*>& list;
        ~BuildInterfaces() {
            for (InterfaceDecl* d : list) r.interfaceInfo(d);
        }
    } buildInterfaces{*this, laterInterfaces};
    for (auto& sp : program->stmts) {
        Stmt* s = sp.get();
        switch (s->kind) {
            case NK::FnDecl: {
                auto* f = static_cast<FnDecl*>(s);
                auto existing = mod.globals.find(f->name);
                if (!repl && existing != mod.globals.end() &&
                    (existing->second->kind == DeclKind::Fn || existing->second->kind == DeclKind::Class))
                    error(f->namePos, "'" + f->name + "' is already defined (line " +
                                          std::to_string(existing->second->pos.line) + ")",
                          (int)f->name.size());
                Decl* d = mod.addGlobal(f->name, DeclKind::Fn, f->namePos);
                d->kind = DeclKind::Fn;
                d->node = f;
                d->pos = f->namePos;
                f->decl = d;
                for (auto& a : f->attrs)
                    if (a == "test") mod.tests.push_back(f);
                break;
            }
            case NK::ClassDecl: {
                auto* c = static_cast<ClassDecl*>(s);
                auto existing = mod.globals.find(c->name);
                if (!repl && existing != mod.globals.end() &&
                    (existing->second->kind == DeclKind::Fn || existing->second->kind == DeclKind::Class))
                    error(c->namePos, "'" + c->name + "' is already defined (line " +
                                          std::to_string(existing->second->pos.line) + ")",
                          (int)c->name.size());
                Decl* d = mod.addGlobal(c->name, DeclKind::Class, c->namePos);
                d->kind = DeclKind::Class;
                d->node = c;
                d->pos = c->namePos;
                c->decl = d;
                break;
            }
            case NK::InterfaceDecl:
            case NK::EnumDecl: {
                bool isIface = s->kind == NK::InterfaceDecl;
                std::string nm = isIface ? static_cast<InterfaceDecl*>(s)->name : static_cast<EnumDecl*>(s)->name;
                SrcPos np = isIface ? static_cast<InterfaceDecl*>(s)->namePos : static_cast<EnumDecl*>(s)->namePos;
                auto existing = mod.globals.find(nm);
                if (!repl && existing != mod.globals.end() && existing->second->kind != DeclKind::Var &&
                    existing->second->kind != DeclKind::Import && existing->second->kind != DeclKind::Module)
                    error(np, "'" + nm + "' is already defined (line " + std::to_string(existing->second->pos.line) + ")",
                          (int)nm.size());
                Decl* d = mod.addGlobal(nm, isIface ? DeclKind::Interface : DeclKind::Enum, np);
                d->kind = isIface ? DeclKind::Interface : DeclKind::Enum;
                d->node = s;
                d->pos = np;
                if (isIface) {
                    static_cast<InterfaceDecl*>(s)->decl = d;
                    laterInterfaces.push_back(static_cast<InterfaceDecl*>(s));
                } else {
                    static_cast<EnumDecl*>(s)->decl = d;
                }
                break;
            }
            case NK::Use: processUse(static_cast<Use*>(s)); break;
            case NK::VarDecl: {
                auto* v = static_cast<VarDecl*>(s);
                Decl* d = mod.addGlobal(v->name, DeclKind::Var, v->pos);
                v->decl = d;
                d->annot = v->annot.get();
                break;
            }
            default: break;
        }
    }
    std::vector<std::pair<std::string, SrcPos>> names;
    for (auto& sp : program->stmts) {
        if (sp->kind == NK::FnDecl || sp->kind == NK::ClassDecl || sp->kind == NK::Use ||
            sp->kind == NK::InterfaceDecl || sp->kind == NK::EnumDecl)
            continue;
        collectAssigned(sp.get(), names);
    }
    for (auto& [n, p] : names) {
        Decl* d = mod.addGlobal(n, DeclKind::Var, p);
        d->assignCount++;
    }
}

void Resolver::resolveModule(Block* program) {
    if (!mod.script) {
        mod.funcs.emplace_back();
        mod.script = &mod.funcs.back();
        mod.script->isModule = true;
        mod.script->module = &mod;
    }
    curFn = mod.script;
    scopes.clear();
    prescanModule(program);
    for (auto& s : program->stmts) resolveStmt(s.get());
}

// ------------------------------------------------------------------ statements

void Resolver::resolveBlock(Block* b, bool newScope) {
    if (!b) return;
    if (newScope) scopes.push_back({{}, curFn});
    for (auto& s : b->stmts) resolveStmt(s.get());
    if (newScope) scopes.pop_back();
}

void Resolver::resolveTarget(Expr* target, bool plainAssign) {
    if (target->kind == NK::Ident) {
        auto* id = static_cast<Ident*>(target);
        if (id->name == "self") {
            error(id->pos, "can't assign to 'self'", 4);
            return;
        }
        if (id->isPlaceholder()) {
            error(id->pos, "placeholders like '" + id->name + "' can't be assigned", (int)id->name.size());
            return;
        }
        if (bindSelfMember(id, true)) return;
        id->decl = lookup(id->name);
        if (!id->decl) {
            std::string msg = "undefined variable '" + id->name + "'";
            if (!plainAssign) msg += " (compound assignment needs an existing variable)";
            error(id->pos, msg, (int)id->name.size());
            return;
        }
        if (id->decl->kind == DeclKind::Builtin) {
            error(id->pos, "can't assign to the builtin '" + id->name + "'", (int)id->name.size());
            return;
        }
        if (id->decl->kind == DeclKind::Import) {
            std::string from = id->decl->importFrom ? id->decl->importFrom->name : "another module";
            error(id->pos, "'" + id->name + "' comes from " + from + "; only " + from + " can change it",
                  (int)id->name.size());
            return;
        }
        id->decl->assignCount++;
        return;
    }
    if (target->kind == NK::Member) {
        resolveExpr(static_cast<Member*>(target)->object.get());
        return;
    }
    if (target->kind == NK::Index) {
        auto* ix = static_cast<Index*>(target);
        resolveExpr(ix->object.get());
        resolveExpr(ix->index.get());
        return;
    }
    error(target->pos, "can't assign to this expression");
}

void Resolver::resolveStmt(Stmt* s) {
    switch (s->kind) {
        case NK::ExprStmt: resolveExpr(static_cast<ExprStmt*>(s)->expr.get()); break;
        case NK::Assign: {
            auto* a = static_cast<Assign*>(s);
            resolveExpr(a->value.get());
            resolveTarget(a->target.get(), a->op == BinOp::None);
            if (a->exported) {
                // limit* = 10: importers can read it (not change it)
                Decl* d = a->target->kind == NK::Ident ? static_cast<Ident*>(a->target.get())->decl : nullptr;
                if (curFn != mod.script || !scopes.empty() || !d || !d->isGlobal)
                    error(a->pos, "only module variables can be exported (limit* = 10 at the top of a file)");
                else
                    d->exported = true;
            }
            if (a->annot && a->target->kind == NK::Ident) {
                Decl* d = static_cast<Ident*>(a->target.get())->decl;
                if (d && !d->annot) d->annot = a->annot.get();
            }
            break;
        }
        case NK::Destructure: {
            auto* d = static_cast<Destructure*>(s);
            resolveExpr(d->value.get());
            for (auto& t : d->targets) resolveTarget(t.get(), true);
            break;
        }
        case NK::VarDecl: {
            auto* v = static_cast<VarDecl*>(s);
            resolveExpr(v->init.get());
            if (v->decl) break;  // module-level var: declared by the prescan
            if (!scopes.empty() && scopes.back().names.count(v->name))
                error(v->pos, "'" + v->name + "' is already declared in this block", (int)v->name.size());
            v->decl = declareLocal(v->name, DeclKind::Var, v->pos, true);
            v->decl->annot = v->annot.get();
            break;
        }
        case NK::IncDec: resolveTarget(static_cast<IncDec*>(s)->target.get(), false); break;
        case NK::Block: resolveBlock(static_cast<Block*>(s)); break;
        case NK::While: {
            auto* w = static_cast<While*>(s);
            resolveExpr(w->cond.get());
            loopDepth++;
            resolveBlock(w->body.get());
            loopDepth--;
            break;
        }
        case NK::For: {
            auto* f = static_cast<For*>(s);
            resolveExpr(f->iter.get());
            resolveExpr(f->step.get());
            scopes.push_back({{}, curFn});
            for (auto& v : f->vars) {
                v.decl = declareLocal(v.name, v.name == "_" ? DeclKind::Hidden : DeclKind::Var, v.pos, true);
            }
            loopDepth++;
            resolveBlock(f->body.get());
            loopDepth--;
            scopes.pop_back();
            break;
        }
        case NK::ForRecv: {
            auto* f = static_cast<ForRecv*>(s);
            for (auto& c : f->chans) resolveExpr(c.get());
            scopes.push_back({{}, curFn});
            for (auto& v : f->vars) {
                v.decl = declareLocal(v.name, v.name == "_" ? DeclKind::Hidden : DeclKind::Var, v.pos, true);
            }
            loopDepth++;
            resolveBlock(f->body.get());
            loopDepth--;
            scopes.pop_back();
            break;
        }
        case NK::Return:
            if (inDefer) error(s->pos, "'return' can't be used inside a defer", 6);
            resolveExpr(static_cast<Return*>(s)->value.get());
            break;
        case NK::Break:
            if (loopDepth == 0) error(s->pos, "'break' outside of a loop", 5);
            break;
        case NK::Continue:
            if (loopDepth == 0) error(s->pos, "'continue' outside of a loop", 8);
            break;
        case NK::Retry: {
            auto* r = static_cast<Retry*>(s);
            resolveExpr(r->count.get());
            resolveExpr(r->delay.get());
            if (catches.empty())
                error(s->pos, "'retry' can only be used inside an error handler (expr ! err ...)", 5);
            else
                catches.back()->hasRetry = true;
            break;
        }
        case NK::FnDecl: {
            auto* f = static_cast<FnDecl*>(s);
            if (!f->decl) {
                f->decl = lookup(f->name);
                if (f->decl) {
                    f->decl->node = f;
                    if (f->decl->kind == DeclKind::Var) f->decl->kind = DeclKind::Fn;
                }
            }
            resolveFunction(f, nullptr);
            break;
        }
        case NK::ClassDecl: {
            auto* c = static_cast<ClassDecl*>(s);
            if (!c->decl) {
                c->decl = lookup(c->name);
                if (c->decl) {
                    c->decl->node = c;
                    if (c->decl->kind == DeclKind::Var) c->decl->kind = DeclKind::Class;
                }
            }
            resolveClass(c);
            break;
        }
        case NK::InterfaceDecl: {
            auto* d = static_cast<InterfaceDecl*>(s);
            if (!d->decl) {
                d->decl = lookup(d->name);
                if (d->decl) {
                    d->decl->node = d;
                    d->decl->kind = DeclKind::Interface;
                }
            }
            {
                InterfaceInfo* savedIface = curIface;
                curIface = interfaceInfo(d);
                for (auto& m : d->methods)
                    if (m->hasBody) resolveFunction(m.get(), nullptr);
                curIface = savedIface;
            }
            break;
        }
        case NK::EnumDecl: {
            auto* e = static_cast<EnumDecl*>(s);
            if (!e->decl) {
                e->decl = lookup(e->name);
                if (e->decl) {
                    e->decl->node = e;
                    e->decl->kind = DeclKind::Enum;
                }
            }
            resolveEnum(e);
            break;
        }
        case NK::Use:
            if (curFn != mod.script || !scopes.empty())
                error(s->pos, "'use' must be at the top level of a file", 3);
            break;
        case NK::With: resolveBlock(static_cast<With*>(s)->body.get()); break;
        case NK::Region: {
            auto* r = static_cast<Region*>(s);
            if (r->isArena()) {
                scopes.push_back({{}, curFn});
                std::vector<std::pair<std::string, SrcPos>> names;
                for (auto& st : r->body->stmts) collectAssigned(st.get(), names);
                for (auto& [n, p] : names) {
                    if (lookup(n, false) && lookup(n, false)->kind != DeclKind::Builtin) continue;
                    if (scopes.back().names.count(n)) continue;
                    Decl* d = declareLocal(n, DeclKind::Var, p, true);
                    d->mayBeUnset = true;
                    r->scopedVars.push_back(d);
                }
                resolveBlock(r->body.get());
                scopes.pop_back();
                break;
            }
            resolveExpr(r->init.get());
            // the object lives in a hidden module variable, one per @cache / @dual site
            std::string hidden = "$region@" + std::to_string(r->pos.line) + ":" + std::to_string(r->pos.col);
            r->storeDecl = mod.addGlobal(hidden, DeclKind::Hidden, r->pos);
            r->storeDecl->kind = DeclKind::Hidden;
            scopes.push_back({{}, curFn});
            r->nameDecl = declareLocal(r->name, DeclKind::Var, r->namePos, true);
            resolveBlock(r->body.get());
            scopes.pop_back();
            break;
        }
        case NK::Select: {
            for (auto& a : static_cast<Select*>(s)->arms) {
                resolveExpr(a.expr.get());
                scopes.push_back({{}, curFn});
                if (a.kind == SelectArm::Recv && !a.name.empty() && a.name != "_")
                    a.decl = declareLocal(a.name, DeclKind::Var, a.namePos, true);
                resolveBlock(a.body.get(), false);
                scopes.pop_back();
            }
            break;
        }
        case NK::Defer: {
            auto* d = static_cast<Defer*>(s);
            int savedLoop = loopDepth;
            auto savedCatches = std::move(catches);
            catches.clear();
            loopDepth = 0;  // break / continue / retry can't leave a defer
            inDefer++;
            resolveBlock(d->body.get());
            inDefer--;
            loopDepth = savedLoop;
            catches = std::move(savedCatches);
            break;
        }
        case NK::Recv: {
            auto* r = static_cast<Recv*>(s);
            resolveExpr(r->chan.get());
            resolveTarget(r->target.get(), true);
            break;
        }
        case NK::Todo: break;
        default: error(s->pos, "internal: unexpected statement"); break;
    }
}

// ------------------------------------------------------------------ functions & classes

void Resolver::resolveFunction(FnDecl* fn, ClassInfo* cls) {
    mod.funcs.emplace_back();
    FuncInfo* info = &mod.funcs.back();
    info->node = fn;
    info->parent = curFn;
    info->module = &mod;
    info->cls = cls;
    fn->info = info;
    fn->cls = cls;

    FuncInfo* savedFn = curFn;
    int savedLoop = loopDepth;
    auto savedCatches = std::move(catches);
    catches.clear();
    loopDepth = 0;
    curFn = info;
    scopes.push_back({{}, info});

    if (cls || fn->selfFirst || fn->isMethod) {
        Decl* self = declareLocal("self", DeclKind::Self, fn->pos, false);
        info->selfDecl = self;
    }
    for (size_t i = 0; i < fn->params.size(); i++) {
        auto& p = fn->params[i];
        if (fn->selfFirst && i == 0) {
            p.decl = info->selfDecl;
            p.decl->annot = p.type.get();
            continue;
        }
        if (p.name == "self") error(p.pos, "'self' is only available in methods", 4);
        p.decl = declareLocal(p.name, DeclKind::Param, p.pos, false);
        p.decl->annot = p.type.get();
        info->params.push_back(p.decl);
    }
    for (auto& p : fn->params) resolveExpr(p.def.get());

    std::vector<std::pair<std::string, SrcPos>> names;
    if (fn->body)
        for (auto& st : fn->body->stmts) collectAssigned(st.get(), names);
    for (auto& [n, pos] : names) {
        if (lookupLocalScopes(n)) continue;  // param, or closure variable of an enclosing function
        if ((cls || fn->isLambda || fn->isMethod) && selfMemberName(n) && lookup("self", false)) continue;  // a field
        Decl* d = declareLocal(n, DeclKind::Var, pos, false);
        info->hoisted.push_back(d);
    }
    if (fn->body) resolveBlock(fn->body.get(), false);

    scopes.pop_back();
    curFn = savedFn;
    loopDepth = savedLoop;
    catches = std::move(savedCatches);
}

static void scanSelfFields(Node* n, ClassInfo& ci) {
    if (!n) return;
    if (n->kind == NK::Lambda) {
        scanSelfFields(static_cast<Lambda*>(n)->fn.get(), ci);
        return;
    }
    if (n->kind == NK::Assign) {
        auto* a = static_cast<Assign*>(n);
        if (a->target->kind == NK::Member) {
            auto* m = static_cast<Member*>(a->target.get());
            if (m->object->kind == NK::Ident && static_cast<Ident*>(m->object.get())->name == "self")
                ci.addField(m->name, a->annot.get());
        }
    }
    forEachChild(n, [&](Node* c) { scanSelfFields(c, ci); });
}

InterfaceInfo* Resolver::interfaceInfo(InterfaceDecl* d) {
    if (d->info) return d->info;
    mod.interfaces.emplace_back();
    InterfaceInfo* ii = &mod.interfaces.back();
    ii->name = d->name;
    ii->module = &mod;
    ii->node = d;
    d->info = ii;  // set early: a cycle of `is` finds it (and stops)
    for (auto& m : d->methods) {
        if (ii->methods.count(m->name)) error(m->namePos, "method '" + m->name + "' is declared twice", (int)m->name.size());
        ii->methods[m->name] = m.get();
        if (!m->hasBody) ii->required.push_back(m->name);
    }
    // interface Solid is Shape: everything Shape has, unless Solid redefines it
    for (auto& [bname, bpos] : d->bases) {
        Decl* bd = lookup(bname, false);
        InterfaceInfo* base = nullptr;
        if (bd && bd->kind == DeclKind::Interface && bd->node)
            base = interfaceInfo(static_cast<InterfaceDecl*>(bd->node));
        else
            base = findInterface(bname);
        if (!base) {
            error(bpos, bd ? "'" + bname + "' is not an interface" : "undefined interface '" + bname + "'", (int)bname.size());
            continue;
        }
        if (base == ii) {
            error(bpos, "an interface can't extend itself", (int)bname.size());
            continue;
        }
        for (auto& [mname, fn] : base->methods) {
            if (ii->methods.count(mname)) continue;
            ii->methods[mname] = fn;
            if (!fn->hasBody) ii->required.push_back(mname);
        }
    }
    return ii;
}

InterfaceInfo* Resolver::findInterface(const std::string& name) {
    Decl* d = lookup(name, false);
    if (!d) return nullptr;
    if (d->kind == DeclKind::Interface && d->node) return interfaceInfo(static_cast<InterfaceDecl*>(d->node));
    if (d->kind == DeclKind::Import && d->importFrom && !d->importFrom->isNative) {
        auto it = d->importFrom->globals.find(d->importName);
        if (it != d->importFrom->globals.end() && it->second->kind == DeclKind::Interface && it->second->node)
            return static_cast<InterfaceDecl*>(it->second->node)->info;
    }
    return nullptr;
}

void Resolver::resolveEnum(EnumDecl* e) {
    mod.enums.emplace_back();
    EnumInfo* ei = &mod.enums.back();
    ei->name = e->name;
    ei->module = &mod;
    ei->node = e;
    e->info = ei;
    for (auto& v : e->variants) {
        for (auto& m : e->methods) v.cls->shared.push_back(m.get());
        resolveClass(v.cls.get());
        v.cls->info->enumOf = ei;
        ei->variantNames.push_back(v.name);
        ei->variants.push_back(v.cls->info);
    }
    EnumDecl* savedEnum = curEnum;
    curEnum = e;
    for (auto& m : e->methods) resolveFunction(m.get(), nullptr);
    curEnum = savedEnum;
}

void Resolver::resolveClass(ClassDecl* c) {
    mod.classes.emplace_back();
    ClassInfo& ci = mod.classes.back();
    ci.name = c->name;
    ci.node = c;
    ci.module = &mod;
    ci.decl = c->decl;
    c->info = &ci;
    for (auto& p : c->ctorParams) ci.positional.push_back(ci.addField(p.name, p.type.get()));
    for (auto& fd : c->fields) {
        if (ci.fieldIndex.count(fd.name) && !c->hasCtorParams)
            error(fd.pos, "field '" + fd.name + "' is declared twice", (int)fd.name.size());
        int idx = ci.addField(fd.name, fd.type.get());
        if (!c->hasCtorParams) ci.positional.push_back(idx);
    }
    for (auto& m : c->methods) {
        if (ci.methods.count(m->name)) {
            error(m->namePos, "method '" + m->name + "' is defined twice", (int)m->name.size());
            continue;
        }
        if (ci.fieldIndex.count(m->name))
            error(m->namePos, "'" + m->name + "' is both a field and a method", (int)m->name.size());
        ci.methods[m->name] = m.get();
        if (m->name == "init") ci.init = m.get();
    }
    ci.typeParams = c->typeParams;
    for (auto& [iname, ipos] : c->interfaces) {
        InterfaceInfo* ii = findInterface(iname);
        if (!ii) {
            Decl* d = lookup(iname, false);
            error(ipos, d ? "'" + iname + "' is not an interface" : "undefined interface '" + iname + "'", (int)iname.size());
            continue;
        }
        ci.interfaces.push_back(ii);
        for (auto& [mname, fn] : ii->methods) {
            if (ci.methods.count(mname)) continue;
            if (!fn->hasBody) {
                error(c->namePos, c->name + " doesn't define '" + mname + "', which " + iname + " requires",
                      (int)c->name.size());
                continue;
            }
            ci.methods[mname] = fn;  // default method from the interface
            c->shared.push_back(fn);
        }
    }
    for (FnDecl* fn : c->shared)
        if (!ci.methods.count(fn->name) || ci.methods[fn->name] == fn) ci.methods[fn->name] = fn;
    // A class's fields: constructor parameters, the class body, and `self.x = `
    // in init. The layout is fixed, so every field has a known slot.
    for (auto& m : c->methods)
        if (m->name == "init") scanSelfFields(m->body.get(), ci);

    // Synthetic constructor: sets field defaults, stores constructor params,
    // then forwards the arguments to init (if any).
    mod.funcs.emplace_back();
    FuncInfo* ctor = &mod.funcs.back();
    ctor->parent = curFn;
    ctor->module = &mod;
    ctor->cls = &ci;
    ci.ctorInfo = ctor;
    FuncInfo* savedFn = curFn;
    curFn = ctor;
    scopes.push_back({{}, ctor});
    ctor->selfDecl = declareLocal("self", DeclKind::Self, c->pos, false);
    if (c->hasCtorParams) {
        if (ci.init && !ci.init->params.empty())
            error(ci.init->namePos,
                  "a class with constructor parameters (class " + c->name + "(...)) can't have an init that takes parameters",
                  4);
        for (auto& p : c->ctorParams) {
            p.decl = declareLocal(p.name, DeclKind::Param, p.pos, false);
            p.decl->annot = p.type.get();
            ctor->params.push_back(p.decl);
        }
        for (auto& p : c->ctorParams) resolveExpr(p.def.get());
    } else if (ci.init) {
        for (size_t i = 0; i < ci.init->params.size(); i++) {
            Decl* d = declareLocal("$arg" + std::to_string(i), DeclKind::Param, ci.init->params[i].pos, false);
            ctor->params.push_back(d);
        }
    }
    for (auto& fd : c->fields) resolveExpr(fd.def.get());
    scopes.pop_back();
    curFn = savedFn;

    ClassInfo* savedClass = curClass;
    curClass = &ci;
    for (auto& m : c->methods) resolveFunction(m.get(), &ci);
    curClass = savedClass;
}

// ------------------------------------------------------------------ expressions

void Resolver::resolvePattern(Pattern* p) {
    if (!p) return;
    switch (p->k) {
        case Pattern::Wildcard: break;
        case Pattern::Bind:
            if (!scopes.empty() && scopes.back().names.count(p->name) &&
                scopes.back().names[p->name]->kind != DeclKind::Hidden)
                error(p->pos, "'" + p->name + "' is bound twice in this pattern", (int)p->name.size());
            p->decl = declareLocal(p->name, DeclKind::Var, p->pos, true);
            break;
        case Pattern::Literal:
        case Pattern::Value: resolveExpr(p->value.get()); break;
        case Pattern::RangeP:
            resolveExpr(p->value.get());
            resolveExpr(p->value2.get());
            break;
        case Pattern::TypeP_: break;
        case Pattern::ClassP:
            resolveExpr(p->value.get());
            for (auto& s : p->subs) resolvePattern(s.get());
            break;
        case Pattern::TupleP:
            for (auto& s : p->subs) resolvePattern(s.get());
            break;
        case Pattern::MapP:
            for (auto& k : p->keys) resolveExpr(k.get());
            for (auto& s : p->subs) resolvePattern(s.get());
            break;
        case Pattern::ListP:
            for (auto& s : p->subs) resolvePattern(s.get());
            if (!p->restName.empty()) p->restDecl = declareLocal(p->restName, DeclKind::Var, p->restPos, true);
            break;
        case Pattern::Alt:
            for (auto& s : p->subs) {
                size_t before = scopes.back().names.size();
                resolvePattern(s.get());
                if (scopes.back().names.size() != before)
                    error(s->pos, "alternative patterns (a | b) can't bind names; use 'as' around the whole pattern");
            }
            break;
        case Pattern::As:
            resolvePattern(p->subs[0].get());
            p->decl = declareLocal(p->name, DeclKind::Var, p->pos, true);
            break;
    }
}

void Resolver::resolveExpr(Expr* e) {
    if (!e) return;
    switch (e->kind) {
        case NK::Ident: {
            auto* id = static_cast<Ident*>(e);
            if (!id->isPlaceholder() && bindSelfMember(id, false)) break;
            id->decl = lookup(id->name);
            if (id->decl) break;
            int len = (int)id->name.size();
            if (id->name == "self") {
                error(id->pos, "'self' can only be used inside a method", 4);
            } else if (id->isPlaceholder()) {
                error(id->pos,
                      "'" + id->name +
                          "' is a lambda placeholder; use it inside a call argument (list.map(_ * 2)) or after '->'",
                      len);
            } else {
                std::string msg = "undefined name '" + id->name + "'";
                std::string s = suggest(id->name);
                if (!s.empty()) msg += " (did you mean '" + s + "'?)";
                error(id->pos, msg, len);
            }
            break;
        }
        case NK::Member: {
            auto* m = static_cast<Member*>(e);
            resolveExpr(m->object.get());
            for (auto& a : m->args) resolveExpr(a.get());
            Decl* d = lookup(m->name, true);
            if (d && d->kind != DeclKind::Class && d->kind != DeclKind::Module) m->ufcs = d;
            break;
        }
        case NK::Lambda: resolveFunction(static_cast<Lambda*>(e)->fn.get(), nullptr); break;
        case NK::NamedArg: resolveExpr(static_cast<NamedArg*>(e)->value.get()); break;
        case NK::Match: {
            auto* m = static_cast<Match*>(e);
            resolveExpr(m->subject.get());
            for (auto& arm : m->arms) {
                scopes.push_back({{}, curFn});
                resolvePattern(arm.pat.get());
                resolveExpr(arm.guard.get());
                resolveBlock(arm.body.get(), false);
                scopes.pop_back();
            }
            break;
        }
        case NK::Catch: {
            auto* c = static_cast<Catch*>(e);
            resolveExpr(c->expr.get());
            scopes.push_back({{}, curFn});
            if (!c->errName.empty() && c->errName != "_")
                c->errDecl = declareLocal(c->errName, DeclKind::Var, c->errPos, true);
            catches.push_back(c);
            resolveBlock(c->handler.get(), false);
            catches.pop_back();
            scopes.pop_back();
            break;
        }
        case NK::If: {
            auto* x = static_cast<If*>(e);
            resolveExpr(x->cond.get());
            resolveBlock(x->then.get());
            resolveBlock(x->otherwise.get());
            break;
        }
        default:
            forEachChild(e, [&](Node* c) {
                if (auto* ce = dynamic_cast<Expr*>(c))
                    resolveExpr(ce);
                else
                    resolveStmt(static_cast<Stmt*>(c));
            });
            break;
    }
}

}  // namespace ixo
