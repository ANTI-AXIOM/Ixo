#include "checker.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "parser.hpp"
#include "program.hpp"

namespace ixo {

// ------------------------------------------------------------------ printing

// Types can be cyclic (fn r(n) = r returns itself): print a few levels deep.
static std::string typeToStringDepth(const Type* t, int depth);

std::string typeToString(const Type* t) { return typeToStringDepth(t, 0); }

static std::string typeToStringDepth(const Type* t, int depth) {
    if (!t) return "nothing";
    if (depth > 6) return "...";
    auto typeToString = [depth](const Type* x) { return typeToStringDepth(x, depth + 1); };
    auto list = [&](const std::vector<Type*>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + typeToString(v[i]);
        return s;
    };
    switch (t->k) {
        case Type::Any: return "any";
        case Type::Nil: return "nil";
        case Type::Bool: return "bool";
        case Type::Int: return "int";
        case Type::Float: return "float";
        case Type::Num: return "num";
        case Type::Str: return "str";
        case Type::List: return "list[" + (t->args.empty() ? std::string("any") : typeToString(t->args[0])) + "]";
        case Type::Set: return "set[" + (t->args.empty() ? std::string("any") : typeToString(t->args[0])) + "]";
        case Type::Flow: return "flow[" + (t->args.empty() ? std::string("any") : typeToString(t->args[0])) + "]";
        case Type::Iter: return "iter";
        case Type::Map:
            return "map[" + (t->args.size() < 2 ? std::string("any, any") : typeToString(t->args[0]) + ", " + typeToString(t->args[1])) + "]";
        case Type::Tuple: return t->args.empty() ? "tuple" : "(" + list(t->args) + (t->args.size() == 1 ? ",)" : ")");
        case Type::Fn: {
            std::string s = "fn(" + list(t->args);
            if (t->maxArgs < 0 && t->arityKnown) s += t->args.empty() ? "..." : ", ...";
            s += ")";
            if (t->ret && t->ret->k != Type::Nil) s += " -> " + typeToString(t->ret);
            return s;
        }
        case Type::Instance: {
            if (!t->cls) return "object";
            if (t->args.empty()) return t->cls->name;
            return t->cls->name + "[" + list(t->args) + "]";
        }
        case Type::Class: return "class " + (t->cls ? t->cls->name : std::string("?"));
        case Type::Range: return "range";
        case Type::Task: return "task";
        case Type::Error: return "Error";
        case Type::Result: return typeToString(t->args[0]) + " ! Error";
        case Type::Optional: return typeToString(t->args[0]) + "?";
        case Type::Vec: return "vec";
        case Type::Mat: return "mat";
        case Type::Quat: return "quat";
        case Type::Rng: return "rng";
        case Type::Time: return "time";
        case Type::Module: return "module " + t->name;
        case Type::Namespace: return t->name;
        case Type::TypeVar: return t->name;
        case Type::Interface: return t->iface ? t->iface->name : "interface";
        case Type::Enum: return t->en ? t->en->name : "enum";
        case Type::EnumDef: return "enum " + (t->en ? t->en->name : std::string("?"));
    }
    return "?";
}

struct TypeArena {
    std::deque<Type> types;
};

// ------------------------------------------------------------------ checker

struct Checker::Impl {
    Program& prog;
    ModuleUnit& mod;
    Diagnostics& diags;
    VM& vm;
    std::shared_ptr<TypeArena> arena;
    bool report = false;

    Type *tAny, *tNil, *tBool, *tInt, *tFloat, *tNum, *tStr, *tRange, *tError, *tVec, *tMat, *tQuat, *tRng, *tTime,
        *tTask, *tIter, *tFnAny;

    struct FnCtx {
        FnDecl* fn;
        Type* declaredRet;
        Type* returned;
    };
    std::vector<FnCtx> fns;
    std::unordered_map<FnDecl*, Type*> fnRet;
    std::unordered_map<FnDecl*, Type*> fnTypes;
    std::unordered_map<ClassInfo*, Type*> instTypes, classTypes;
    std::unordered_map<ClassInfo*, std::vector<Type*>> fieldInferred;
    std::unordered_map<std::string, Type*> sigCache;
    std::unordered_map<int, Type*> builtinTypes;
    std::unordered_set<std::string> reported;

    Impl(Program& p, ModuleUnit& m, Diagnostics& d) : prog(p), mod(m), diags(d), vm(p.vm) {
        // Reuse the module's arena: declarations from earlier REPL inputs point into it.
        if (mod.typeArena)
            arena = std::static_pointer_cast<TypeArena>(mod.typeArena);
        else
            arena = std::make_shared<TypeArena>();
        mod.typeArena = arena;
        tAny = mk(Type::Any);
        tNil = mk(Type::Nil);
        tBool = mk(Type::Bool);
        tInt = mk(Type::Int);
        tFloat = mk(Type::Float);
        tNum = mk(Type::Num);
        tStr = mk(Type::Str);
        tRange = mk(Type::Range);
        tError = mk(Type::Error);
        tVec = mk(Type::Vec);
        tMat = mk(Type::Mat);
        tQuat = mk(Type::Quat);
        tRng = mk(Type::Rng);
        tTime = mk(Type::Time);
        tTask = mk(Type::Task);
        tIter = mk(Type::Iter);
        tFnAny = mk(Type::Fn);
    }

    // ---------------------------------------------------------- construction

    Type* mk(Type::K k) {
        arena->types.emplace_back();
        Type* t = &arena->types.back();
        t->k = k;
        return t;
    }
    Type* mk1(Type::K k, Type* a) {
        Type* t = mk(k);
        t->args.push_back(a ? a : tAny);
        return t;
    }
    Type* listOf(Type* e) { return mk1(Type::List, e); }
    Type* optional(Type* t) {
        if (!t || t->k == Type::Any || t->k == Type::Optional || t->k == Type::Nil) return t;
        return mk1(Type::Optional, t);
    }
    Type* result(Type* t) {
        if (!t || t->k == Type::Any || t->k == Type::Result || t->k == Type::Error) return t;
        return mk1(Type::Result, t);
    }
    static bool isVague(const Type* t) { return !t || t->k == Type::Any || t->k == Type::TypeVar; }
    static Type* strip(Type* t) {
        while (t && (t->k == Type::Optional || t->k == Type::Result)) t = t->args[0];
        return t;
    }

    void warn(SrcPos p, const std::string& msg, int len = 1) {
        if (!report) return;
        std::string key = std::to_string(p.line) + ":" + std::to_string(p.col) + msg;
        if (!reported.insert(key).second) return;
        diags.warning(mod.path, p, msg, len);
    }

    void deprecated(SrcPos p, const std::string& msg, std::vector<TextEdit> fix, int len = 1) {
        if (!report) return;
        std::string key = std::to_string(p.line) + ":" + std::to_string(p.col) + msg;
        if (!reported.insert(key).second) return;
        diags.deprecated(mod.path, p, msg, len, std::move(fix));
    }

    void err(SrcPos p, const std::string& msg0, int len = 1) {
        if (!report) return;
        std::string msg = msg0;
        // calls to par fn / fork fn give tasks: say how to get the value
        if (msg.find("await") == std::string::npos &&
            (msg.find(" task and ") != std::string::npos || msg.find(" and task") != std::string::npos ||
             msg.find("assign a task") != std::string::npos || msg.find("got task") != std::string::npos ||
             msg.rfind("task has no", 0) == 0))
            msg += " (this is a task: .await gives its result)";
        std::string key = std::to_string(p.line) + ":" + std::to_string(p.col) + msg;
        if (!reported.insert(key).second) return;
        diags.error(mod.path, p, msg, len);
    }

    // ---------------------------------------------------------- type expressions

    ClassInfo* findClass(const std::string& name) {
        auto it = mod.globals.find(name);
        if (it != mod.globals.end()) {
            Decl* d = it->second;
            if (d->kind == DeclKind::Class && d->node) return static_cast<ClassDecl*>(d->node)->info;
            if (d->kind == DeclKind::Import && d->importFrom && !d->importFrom->isNative) {
                auto jt = d->importFrom->globals.find(d->importName);
                if (jt != d->importFrom->globals.end() && jt->second->kind == DeclKind::Class && jt->second->node)
                    return static_cast<ClassDecl*>(jt->second->node)->info;
            }
        }
        for (auto& ci : mod.classes)
            if (ci.name == name) return &ci;
        return nullptr;
    }

    // ---------------------------------------------------------- borrows
    //
    // `p: &Point` is a read-only parameter: the function may look at p but not
    // change it (its fields, elements or entries). `&mut Point` allows changes.

    static bool readOnly(Decl* d) {
        return d && d->kind == DeclKind::Param && d->annot && d->annot->k == TypeExpr::Ref && !d->annot->mut;
    }
    // The variable an expression like p.items[0].name starts from.
    static Ident* rootIdent(Expr* e) {
        while (e) {
            if (e->kind == NK::Ident) return static_cast<Ident*>(e);
            if (e->kind == NK::Member) e = static_cast<Member*>(e)->object.get();
            else if (e->kind == NK::Index) e = static_cast<Index*>(e)->object.get();
            else return nullptr;
        }
        return nullptr;
    }
    void borrowError(Ident* id, SrcPos pos, const std::string& what) {
        std::string ty = id->decl->annot->args.empty() ? "" : id->decl->annot->args[0]->str();
        err(pos, "'" + id->name + "' is borrowed read-only (" + id->name + ": &" + ty + "), so " + what +
                     "; declare it `&mut " + ty + "` to allow changes",
            (int)id->name.size());
    }
    static bool mutatingBuiltin(Type::K k, const std::string& n) {
        static const std::unordered_set<std::string> list = {"add",  "push", "insert", "set",  "drop", "remove",
                                                             "take", "pop",  "sort!",  "rev!", "wipe", "clear"};
        static const std::unordered_set<std::string> mapSet = {"set", "add", "drop", "merge", "wipe", "clear"};
        if (k == Type::List) return list.count(n) > 0;
        if (k == Type::Map || k == Type::Set) return mapSet.count(n) > 0;
        return false;
    }
    // Does a method change `self` (assign its fields, or change them through
    // mutating calls, or call another method that does)?
    std::unordered_map<FnDecl*, int> mutatesMemo;  // 0 unknown/in progress, 1 no, 2 yes
    bool mutatesSelf(FnDecl* fn, ClassInfo* ci) {
        if (!fn || !fn->body) return false;
        int& memo = mutatesMemo[fn];
        if (memo) return memo == 2;
        memo = 1;  // assume not while we look (recursion)
        bool found = false;
        auto selfRooted = [&](Expr* e) {
            Ident* r = rootIdent(e);
            return r && r->name == "self" && e->kind != NK::Ident;
        };
        std::function<void(Node*)> walk = [&](Node* n) {
            if (!n || found) return;
            if (n->kind == NK::Assign && selfRooted(static_cast<Assign*>(n)->target.get())) found = true;
            if (n->kind == NK::IncDec && selfRooted(static_cast<IncDec*>(n)->target.get())) found = true;
            if (n->kind == NK::Member) {
                auto* m = static_cast<Member*>(n);
                if (m->isCall) {
                    Ident* r = rootIdent(m->object.get());
                    if (r && r->name == "self") {
                        if (m->object->kind == NK::Ident) {
                            auto it = ci->methods.find(m->name);
                            if (it != ci->methods.end() && it->second != fn && mutatesSelf(it->second, ci)) found = true;
                        } else {
                            Type* t = strip(m->object->type);
                            if (t && mutatingBuiltin(t->k, m->name)) found = true;
                            if (!t || isVague(t)) {
                                static const std::unordered_set<std::string> any = {"add",  "push", "insert", "set",
                                                                                    "drop", "remove", "take", "pop",
                                                                                    "sort!", "rev!", "wipe", "clear",
                                                                                    "merge"};
                                if (any.count(m->name)) found = true;
                            }
                        }
                    }
                }
            }
            if (n->kind == NK::FnDecl && n != fn) return;  // nested functions run later
            forEachChild(n, walk);
        };
        walk(fn->body.get());
        memo = found ? 2 : 1;
        return found;
    }

    // ---------------------------------------------------------- narrowing
    //
    // Inside `if x != nil`, after `if x == nil -> return`, in `x != nil and x.len > 0`,
    // under `if x is Point` and in a `Point(a, b)` match arm, `x` has a narrower type.
    // Each block has a scope of narrowed variables; assigning a variable forgets them.

    std::vector<std::unordered_map<Decl*, Type*>> narrowed{{}};
    using Facts = std::vector<std::pair<Decl*, Type*>>;
    struct CondFacts {
        Facts yes, no;  // what holds when the condition is true / false
    };

    Type* narrowedType(Decl* d) {
        for (auto it = narrowed.rbegin(); it != narrowed.rend(); ++it) {
            auto f = it->find(d);
            if (f != it->end()) return f->second;
            if (it->count(nullptr)) break;  // a function boundary
        }
        return nullptr;
    }
    void pushFacts(const Facts& fs) {
        narrowed.emplace_back();
        for (auto& [d, t] : fs) narrowed.back()[d] = t;
    }
    void addFacts(const Facts& fs) {
        for (auto& [d, t] : fs) narrowed.back()[d] = t;
    }
    void forget(Decl* d) {
        for (auto& sc : narrowed) sc.erase(d);
    }
    static Type* nonNil(Type* t) {
        while (t && t->k == Type::Optional) t = t->args[0];
        return t;
    }
    static Type* truthyType(Type* t) {
        while (t && (t->k == Type::Optional || t->k == Type::Result)) t = t->args[0];
        return t;
    }
    Decl* narrowable(Expr* e) {
        if (!e || e->kind != NK::Ident) return nullptr;
        Decl* d = static_cast<Ident*>(e)->decl;
        if (!d || (d->kind != DeclKind::Var && d->kind != DeclKind::Param)) return nullptr;
        return d;
    }
    Type* currentType(Decl* d) {
        Type* n = narrowedType(d);
        return n ? n : declType(d);
    }
    // The type a class / interface / enum name stands for in `x is Name`.
    Type* typeNamed(Expr* e) {
        if (!e || e->kind != NK::Ident) return nullptr;
        Decl* d = static_cast<Ident*>(e)->decl;
        if (!d) return nullptr;
        if (d->kind == DeclKind::Import && d->importFrom && !d->importFrom->isNative) {
            auto it = d->importFrom->globals.find(d->importName);
            d = it == d->importFrom->globals.end() ? nullptr : it->second;
            if (!d) return nullptr;
        }
        if (d->kind == DeclKind::Class && d->node && static_cast<ClassDecl*>(d->node)->info)
            return instType(static_cast<ClassDecl*>(d->node)->info);
        if (d->kind == DeclKind::Interface && d->node && static_cast<InterfaceDecl*>(d->node)->info)
            return ifaceType(static_cast<InterfaceDecl*>(d->node)->info);
        if (d->kind == DeclKind::Enum && d->node && static_cast<EnumDecl*>(d->node)->info)
            return enumType(static_cast<EnumDecl*>(d->node)->info);
        return nullptr;
    }
    CondFacts factsOf(Expr* c) {
        CondFacts f;
        if (!c) return f;
        if (c->kind == NK::Unary && static_cast<Unary*>(c)->op == UnOp::Not) {
            CondFacts g = factsOf(static_cast<Unary*>(c)->operand.get());
            std::swap(g.yes, g.no);
            return g;
        }
        if (c->kind == NK::Ident) {
            if (Decl* d = narrowable(c)) {
                Type* t = currentType(d);
                Type* n = truthyType(t);
                if (n && n != t) f.yes.push_back({d, n});
            }
            return f;
        }
        if (c->kind != NK::Binary) return f;
        auto* b = static_cast<Binary*>(c);
        switch (b->op) {
            case BinOp::And: {
                CondFacts l = factsOf(b->left.get());
                pushFacts(l.yes);
                CondFacts r = factsOf(b->right.get());
                narrowed.pop_back();
                f.yes = l.yes;
                f.yes.insert(f.yes.end(), r.yes.begin(), r.yes.end());
                return f;
            }
            case BinOp::Or: {
                CondFacts l = factsOf(b->left.get());
                pushFacts(l.no);
                CondFacts r = factsOf(b->right.get());
                narrowed.pop_back();
                f.no = l.no;
                f.no.insert(f.no.end(), r.no.begin(), r.no.end());
                return f;
            }
            case BinOp::Eq:
            case BinOp::Ne:
            case BinOp::Is:
            case BinOp::IsNot: {
                Expr* x = b->left.get();
                Expr* y = b->right.get();
                if (x->kind == NK::NilLit) std::swap(x, y);
                bool negated = b->op == BinOp::Ne || b->op == BinOp::IsNot;
                Decl* d = narrowable(x);
                if (!d) return f;
                if (y->kind == NK::NilLit) {
                    Type* t = currentType(d);
                    Type* nn = nonNil(t);
                    if (nn && nn != t) f.no.push_back({d, nn});
                    f.yes.push_back({d, tNil});
                } else if (b->op == BinOp::Is || b->op == BinOp::IsNot) {
                    if (Type* target = typeNamed(y)) f.yes.push_back({d, target});
                }
                if (negated) std::swap(f.yes, f.no);
                return f;
            }
            default: return f;
        }
    }

    // A global (or imported) declaration of the given kind, followed through imports.
    Decl* findDecl(const std::string& name, DeclKind kind) {
        auto it = mod.globals.find(name);
        if (it == mod.globals.end()) return nullptr;
        Decl* d = it->second;
        if (d->kind == DeclKind::Import && d->importFrom && !d->importFrom->isNative) {
            auto jt = d->importFrom->globals.find(d->importName);
            if (jt == d->importFrom->globals.end()) return nullptr;
            d = jt->second;
        }
        return d->kind == kind && d->node ? d : nullptr;
    }
    InterfaceInfo* findInterface(const std::string& name) {
        Decl* d = findDecl(name, DeclKind::Interface);
        return d ? static_cast<InterfaceDecl*>(d->node)->info : nullptr;
    }
    EnumInfo* findEnum(const std::string& name) {
        Decl* d = findDecl(name, DeclKind::Enum);
        return d ? static_cast<EnumDecl*>(d->node)->info : nullptr;
    }
    Type* ifaceType(InterfaceInfo* ii) {
        Type* t = mk(Type::Interface);
        t->iface = ii;
        return t;
    }
    Type* enumType(EnumInfo* ei, Type::K k = Type::Enum) {
        Type* t = mk(k);
        t->en = ei;
        return t;
    }
    // Does the class have everything the interface requires?
    static bool satisfies(ClassInfo* ci, InterfaceInfo* ii) {
        if (!ci || !ii) return true;
        for (auto& r : ii->required)
            if (!ci->methods.count(r)) return false;
        return true;
    }
    // The method a value's type provides for an operator ("+", "<", "[]", ...), if any.
    FnDecl* opMethod(Type* t, const std::string& name) {
        t = strip(t);
        if (!t) return nullptr;
        auto find = [&](auto& table) -> FnDecl* {
            auto it = table.find(name);
            return it == table.end() ? nullptr : it->second;
        };
        if (t->k == Type::Instance && t->cls) return find(t->cls->methods);
        if (t->k == Type::Interface && t->iface) return find(t->iface->methods);
        if (t->k == Type::Enum && t->en && t->en->node)
            for (auto& m : t->en->node->methods)
                if (m->name == name) return m.get();
        return nullptr;
    }
    Type* opResult(FnDecl* f, const std::vector<Type*>& args, SrcPos pos, const std::string& name) {
        std::vector<Expr*> none;
        return checkCall(fnType(f), args, none, pos, name);
    }

    Type* fromTypeExpr(TypeExpr* te) {
        if (!te) return tAny;
        switch (te->k) {
            case TypeExpr::Optional: {
                Type* inner = fromTypeExpr(te->args[0].get());
                Type* t = optional(inner);
                if (t != inner && t->k == Type::Optional) t->declared = true;
                return t;
            }
            case TypeExpr::Result: return result(fromTypeExpr(te->args[0].get()));
            case TypeExpr::Ref: return fromTypeExpr(te->args[0].get());
            case TypeExpr::Tuple: {
                Type* t = mk(Type::Tuple);
                for (auto& a : te->args) t->args.push_back(fromTypeExpr(a.get()));
                return t;
            }
            case TypeExpr::Fn: {
                Type* t = mk(Type::Fn);
                for (auto& a : te->args) t->args.push_back(fromTypeExpr(a.get()));
                t->ret = te->ret ? fromTypeExpr(te->ret.get()) : tNil;
                t->minArgs = t->maxArgs = (int)te->args.size();
                t->arityKnown = false;
                return t;
            }
            case TypeExpr::Named: break;
        }
        const std::string& n = te->name;
        auto arg = [&](size_t i) { return i < te->args.size() ? fromTypeExpr(te->args[i].get()) : tAny; };
        if (n == "any") return tAny;
        if (n == "int") return tInt;
        if (n == "float") return tFloat;
        if (n == "num") return tNum;
        if (n == "str") return tStr;
        if (n == "bool") return tBool;
        if (n == "nil" || n == "void") return tNil;
        if (n == "list") return listOf(arg(0));
        if (n == "set") return mk1(Type::Set, arg(0));
        if (n == "flow") return mk1(Type::Flow, arg(0));
        if (n == "iter") return tIter;
        if (n == "map") {
            Type* t = mk(Type::Map);
            t->args = {arg(0), arg(1)};
            return t;
        }
        if (n == "tuple") return mk(Type::Tuple);
        if (n == "fn") return tFnAny;
        if (n == "range") return tRange;
        if (n == "task") return tTask;
        if (n == "vec") return tVec;
        if (n == "mat") return tMat;
        if (n == "quat") return tQuat;
        if (n == "rng") return tRng;
        if (n == "time") return tTime;
        if (n == "Error" || n == "err") return tError;
        if (te->typeVar || (n.size() == 1 && std::isupper((unsigned char)n[0]))) {
            Type* t = mk(Type::TypeVar);
            t->name = n;
            return t;
        }
        if (ClassInfo* ci = findClass(n)) {
            if (!ci->typeParams.empty() && !te->args.empty()) {
                if (te->args.size() != ci->typeParams.size())
                    err(te->pos, ci->name + " takes " + std::to_string(ci->typeParams.size()) + " type argument" +
                                     (ci->typeParams.size() == 1 ? "" : "s"),
                        (int)n.size());
                Type* t = mk(Type::Instance);
                t->cls = ci;
                for (size_t i = 0; i < ci->typeParams.size(); i++) t->args.push_back(arg(i));
                return t;
            }
            return instType(ci);
        }
        if (InterfaceInfo* ii = findInterface(n)) return ifaceType(ii);
        if (EnumInfo* ei = findEnum(n)) return enumType(ei);
        err(te->pos, "unknown type '" + n + "'", (int)n.size());
        return tAny;
    }

    Type* sigType(const std::string& sig) {
        if (sig.empty()) return tAny;
        auto it = sigCache.find(sig);
        if (it != sigCache.end()) return it->second;
        bool variadic = false;
        TypeP te = Parser::parseTypeText(sig, &variadic);
        bool saved = report;
        report = false;  // unknown names in builtin signatures are not the user's fault
        Type* t = te ? fromTypeExpr(te.get()) : tAny;
        report = saved;
        if (t->k == Type::Fn) {
            Type* copy = mk(Type::Fn);
            *copy = *t;
            copy->arityKnown = true;
            copy->minArgs = (int)copy->args.size();
            copy->maxArgs = variadic ? -1 : (int)copy->args.size();
            t = copy;
        }
        sigCache[sig] = t;
        return t;
    }

    // Replaces type variables using bindings (T -> int, ...).
    Type* subst(Type* t, const std::unordered_map<std::string, Type*>& b, int depth = 0) {
        if (!t || b.empty() || depth > 8) return t;  // types can be cyclic
        if (t->k == Type::TypeVar) {
            auto it = b.find(t->name);
            return it == b.end() ? tAny : it->second;
        }
        if (t->args.empty() && !t->ret) return t;
        Type* c = mk(t->k);
        *c = *t;
        for (auto& a : c->args) a = subst(a, b, depth + 1);
        if (c->ret) c->ret = subst(c->ret, b, depth + 1);
        return c;
    }

    void bindVars(Type* param, Type* arg, std::unordered_map<std::string, Type*>& b) {
        if (!param || !arg) return;
        if (param->k == Type::TypeVar) {
            if (!b.count(param->name) && !isVague(arg)) b[param->name] = arg;
            return;
        }
        arg = strip(arg);
        if (arg && param->k == arg->k)
            for (size_t i = 0; i < param->args.size() && i < arg->args.size(); i++) bindVars(param->args[i], arg->args[i], b);
    }

    // ---------------------------------------------------------- compatibility

    bool compatible(Type* src, Type* dst) {
        if (!src || !dst) return true;
        if (isVague(src) || isVague(dst)) return true;
        if (dst->k == Type::Optional) {
            if (src->k == Type::Nil) return true;
            if (src->k == Type::Optional) return compatible(src->args[0], dst->args[0]);
            return compatible(src, dst->args[0]);
        }
        if (dst->k == Type::Result) {
            if (src->k == Type::Error) return true;
            if (src->k == Type::Result) return compatible(src->args[0], dst->args[0]);
            return compatible(src, dst->args[0]);
        }
        if (src->k == Type::Optional || src->k == Type::Result) return compatible(src->args[0], dst);
        if (dst->k == Type::Float && src->k == Type::Int) return true;
        if (dst->k == Type::Num && (src->k == Type::Int || src->k == Type::Float)) return true;
        if (src->k == Type::Num && (dst->k == Type::Int || dst->k == Type::Float)) return true;
        if (dst->k == Type::Iter)
            return src->k == Type::List || src->k == Type::Tuple || src->k == Type::Str || src->k == Type::Map ||
                   src->k == Type::Set || src->k == Type::Range || src->k == Type::Flow || src->k == Type::Iter;
        if (dst->k == Type::Fn) return src->k == Type::Fn || src->k == Type::Class || src->k == Type::Namespace;
        if (dst->k == Type::Interface) {
            if (src->k == Type::Instance) return satisfies(src->cls, dst->iface);
            if (src->k == Type::Interface) return true;
            if (src->k == Type::Enum && src->en) {
                for (ClassInfo* v : src->en->variants)
                    if (!satisfies(v, dst->iface)) return false;
                return true;
            }
            return false;
        }
        if (dst->k == Type::Enum) {
            if (src->k == Type::Instance) return !src->cls || !dst->en || src->cls->enumOf == dst->en;
            if (src->k == Type::Enum) return src->en == dst->en;
            return src->k == Type::Interface;
        }
        // an interface or enum value may turn out to be the expected class
        if (dst->k == Type::Instance && (src->k == Type::Interface || src->k == Type::Enum)) return true;
        if (src->k != dst->k) return false;
        switch (src->k) {
            case Type::List:
            case Type::Set:
            case Type::Flow: return compatible(src->args[0], dst->args[0]);
            case Type::Map: return compatible(src->args[0], dst->args[0]) && compatible(src->args[1], dst->args[1]);
            case Type::Tuple:
                if (src->args.empty() || dst->args.empty()) return true;
                if (src->args.size() != dst->args.size()) return false;
                for (size_t i = 0; i < src->args.size(); i++)
                    if (!compatible(src->args[i], dst->args[i])) return false;
                return true;
            case Type::Instance: {
                bool same = src->cls == dst->cls || (src->cls && dst->cls && src->cls->name == dst->cls->name);
                if (!same) return false;
                if (src->args.size() == dst->args.size())
                    for (size_t i = 0; i < src->args.size(); i++)
                        if (!compatible(src->args[i], dst->args[i])) return false;
                return true;
            }
            default: return true;
        }
    }

    Type* join(Type* a, Type* b) {
        if (!a) return b;
        if (!b) return a;
        if (a == b) return a;
        if (a->k == Type::Any || b->k == Type::Any) return tAny;
        if (a->k == Type::Nil) return b->k == Type::Nil ? a : optional(b);
        if (b->k == Type::Nil) return optional(a);
        if (a->k == Type::Error) return b->k == Type::Error ? a : result(b);
        if (b->k == Type::Error) return result(a);
        if (a->k == Type::Optional || b->k == Type::Optional) {
            Type* x = a->k == Type::Optional ? a->args[0] : a;
            Type* y = b->k == Type::Optional ? b->args[0] : b;
            return optional(join(x, y));
        }
        if (a->k == Type::Result || b->k == Type::Result) {
            Type* x = a->k == Type::Result ? a->args[0] : a;
            Type* y = b->k == Type::Result ? b->args[0] : b;
            return result(join(x, y));
        }
        if (a->k == b->k) {
            switch (a->k) {
                case Type::List:
                case Type::Set:
                case Type::Flow: {
                    Type* e = join(a->args[0], b->args[0]);
                    return e == a->args[0] ? a : mk1(a->k, e);
                }
                case Type::Map: {
                    Type* k = join(a->args[0], b->args[0]);
                    Type* v = join(a->args[1], b->args[1]);
                    if (k == a->args[0] && v == a->args[1]) return a;
                    Type* t = mk(Type::Map);
                    t->args = {k, v};
                    return t;
                }
                case Type::Tuple: {
                    if (a->args.size() != b->args.size()) return mk(Type::Tuple);
                    bool same = true;
                    std::vector<Type*> items;
                    for (size_t i = 0; i < a->args.size(); i++) {
                        items.push_back(join(a->args[i], b->args[i]));
                        if (items.back() != a->args[i]) same = false;
                    }
                    if (same) return a;
                    Type* t = mk(Type::Tuple);
                    t->args = items;
                    return t;
                }
                case Type::Instance:
                    if (a->cls == b->cls) {
                        bool aVague = true;
                        for (Type* x : a->args)
                            if (!isVague(x)) aVague = false;
                        return aVague && !b->args.empty() ? b : a;
                    }
                    if (a->cls && b->cls && a->cls->enumOf && a->cls->enumOf == b->cls->enumOf)
                        return enumType(a->cls->enumOf);
                    return tAny;
                case Type::Enum: return a->en == b->en ? a : tAny;
                default: return a;
            }
        }
        auto numeric = [](Type* t) { return t->k == Type::Int || t->k == Type::Float || t->k == Type::Num; };
        if (numeric(a) && numeric(b)) return (a->k == Type::Num || b->k == Type::Num) ? tNum : tFloat;
        // a variant and its enum
        for (auto [x, y] : {std::pair{a, b}, std::pair{b, a}})
            if (x->k == Type::Enum && y->k == Type::Instance && y->cls && y->cls->enumOf == x->en) return x;
        return tAny;
    }

    Type* elemOf(Type* t) {
        t = strip(t);
        if (!t) return tAny;
        switch (t->k) {
            case Type::List:
            case Type::Set:
            case Type::Flow:
            case Type::Map: return t->args.empty() ? tAny : t->args[0];
            case Type::Str: return tStr;
            case Type::Range: return tInt;
            case Type::Tuple: {
                Type* j = nullptr;
                for (Type* a : t->args) j = join(j, a);
                return j ? j : tAny;
            }
            default: return tAny;
        }
    }

    // ---------------------------------------------------------- declarations

    Type* fnType(FnDecl* f) {
        Type* t;
        auto it = fnTypes.find(f);
        if (it == fnTypes.end()) {
            t = mk(Type::Fn);
            t->fn = f;
            t->name = f->name;
            int minA = 0, n = 0;
            for (size_t i = f->selfFirst ? 1 : 0; i < f->params.size(); i++) {
                auto& p = f->params[i];
                t->args.push_back(p.type ? fromTypeExpr(p.type.get()) : tAny);
                n++;
                if (!p.def) minA = n;
            }
            t->minArgs = minA;
            t->maxArgs = n;
            t->arityKnown = !f->lenientArity;
            fnTypes[f] = t;
        } else {
            t = it->second;
        }
        if (f->isFork || f->isPar)
            t->ret = tTask;  // calling a fork fn / par fn starts a task
        else if (f->ret)
            t->ret = fromTypeExpr(f->ret.get());
        else {
            auto r = fnRet.find(f);
            t->ret = r == fnRet.end() ? tAny : r->second;
        }
        return t;
    }

    Type* instType(ClassInfo* ci) {
        auto it = instTypes.find(ci);
        if (it != instTypes.end()) return it->second;
        Type* t = mk(Type::Instance);
        t->cls = ci;
        instTypes[ci] = t;
        return t;
    }

    Type* classType(ClassInfo* ci) {
        auto it = classTypes.find(ci);
        if (it != classTypes.end()) return it->second;
        Type* t = mk(Type::Class);
        t->cls = ci;
        classTypes[ci] = t;
        ClassDecl* c = ci->node;
        int minA = 0, n = 0;
        if (c && c->hasCtorParams) {
            for (auto& p : c->ctorParams) {
                t->args.push_back(p.type ? fromTypeExpr(p.type.get()) : tAny);
                n++;
                if (!p.def) minA = n;
            }
        } else if (ci->init) {
            for (auto& p : ci->init->params) {
                t->args.push_back(p.type ? fromTypeExpr(p.type.get()) : tAny);
                n++;
                if (!p.def) minA = n;
            }
        }
        t->minArgs = minA;
        t->maxArgs = n;
        t->arityKnown = true;
        if (ci->typeParams.empty()) {
            t->ret = instType(ci);
        } else {
            // Box(5) is a Box[int]: the call binds the type parameters
            Type* r = mk(Type::Instance);
            r->cls = ci;
            for (auto& tp : ci->typeParams) {
                Type* v = mk(Type::TypeVar);
                v->name = tp;
                r->args.push_back(v);
            }
            t->ret = r;
        }
        return t;
    }

    // T -> int for a Box[int]
    std::unordered_map<std::string, Type*> typeArgsOf(Type* inst) {
        std::unordered_map<std::string, Type*> b;
        if (inst && inst->k == Type::Instance && inst->cls)
            for (size_t i = 0; i < inst->cls->typeParams.size() && i < inst->args.size(); i++)
                if (!isVague(inst->args[i])) b[inst->cls->typeParams[i]] = inst->args[i];
        return b;
    }

    Type* nativeArity(Type* t, Value v, bool isMethod) {
        if (t->k != Type::Fn) return t;
        Type* c = mk(Type::Fn);
        *c = *t;
        if (isObjType(v, OT::Native)) {
            auto* n = as<ObjNative>(v);
            int off = isMethod ? 1 : 0;
            c->minArgs = n->minArity - off;
            c->maxArgs = n->maxArity < 0 ? -1 : n->maxArity - off;
            c->arityKnown = true;
        } else if (isObjType(v, OT::Closure)) {
            auto* f = as<ObjClosure>(v)->fn;
            c->minArgs = f->minArity;
            c->maxArgs = f->arity;
            c->arityKnown = true;
        }
        return c;
    }

    Type* builtinType(int idx) {
        auto it = builtinTypes.find(idx);
        if (it != builtinTypes.end()) return it->second;
        BuiltinInfo& b = vm.builtins[idx];
        Type* t;
        if (isObjType(b.value, OT::Namespace)) {
            t = mk(Type::Namespace);
            t->name = b.name;
        } else {
            t = nativeArity(sigType(b.sig), b.value, false);
            if (t->k == Type::Fn && t == sigCache[b.sig]) {
                Type* c = mk(Type::Fn);
                *c = *t;
                t = c;
            }
            if (t->k == Type::Fn) t->name = b.name;
        }
        builtinTypes[idx] = t;
        return t;
    }

    Type* nsMember(const std::string& ns, const std::string& name) {
        auto it = vm.memberDocs.find(ns + "." + name);
        int idx = vm.builtinIndex(ns);
        Value v;
        if (idx >= 0 && isObjType(vm.builtins[idx].value, OT::Namespace)) {
            auto* o = as<ObjNamespace>(vm.builtins[idx].value);
            auto m = o->members.find(name);
            if (m != o->members.end()) v = m->second;
        }
        if (it == vm.memberDocs.end()) return nullptr;
        Type* t = nativeArity(sigType(it->second.first), v, false);
        if (t->k == Type::Fn) {
            Type* c = mk(Type::Fn);
            *c = *t;
            c->name = ns + "." + name;
            t = c;
        }
        return t;
    }

    Type* moduleType(ModuleUnit* m) {
        if (!m) return tAny;
        Type* t = mk(m->isNative ? Type::Namespace : Type::Module);
        t->name = m->name;
        t->module = m;
        return t;
    }

    Type* declType(Decl* d) {
        if (!d) return tAny;
        if (d->annot && d->kind != DeclKind::Fn && d->kind != DeclKind::Class) {
            if (!d->typeFixed) {
                d->type = fromTypeExpr(d->annot);
                d->typeFixed = true;
            }
            return d->type;
        }
        switch (d->kind) {
            case DeclKind::Fn:
                if (d->node && d->node->kind == NK::FnDecl && d->assignCount <= 1)
                    return fnType(static_cast<FnDecl*>(d->node));
                return d->type ? d->type : tAny;
            case DeclKind::Class:
                if (d->node && d->node->kind == NK::ClassDecl && static_cast<ClassDecl*>(d->node)->info)
                    return classType(static_cast<ClassDecl*>(d->node)->info);
                return tAny;
            case DeclKind::Builtin: return builtinType(d->builtinIndex);
            case DeclKind::Enum:
                if (d->node && static_cast<EnumDecl*>(d->node)->info)
                    return enumType(static_cast<EnumDecl*>(d->node)->info, Type::EnumDef);
                return tAny;
            case DeclKind::Import: {
                ModuleUnit* src = d->importFrom;
                if (!src) return tAny;
                if (src->isNative) {
                    Type* t = nsMember(src->name, d->importName);
                    return t ? t : tAny;
                }
                auto it = src->globals.find(d->importName);
                if (it == src->globals.end()) return tAny;
                Decl* sd = it->second;
                if (sd->kind == DeclKind::Fn && sd->node) return fnTypeForeign(static_cast<FnDecl*>(sd->node));
                if (sd->kind == DeclKind::Class && sd->node && static_cast<ClassDecl*>(sd->node)->info)
                    return classType(static_cast<ClassDecl*>(sd->node)->info);
                if (sd->kind == DeclKind::Enum) return declType(sd);
                return sd->type ? sd->type : tAny;
            }
            case DeclKind::Module: return moduleType(d->importFrom);
            case DeclKind::Self:
                if (d->owner && d->owner->cls) return instType(d->owner->cls);
                return tAny;
            default: return d->type ? d->type : tAny;
        }
    }

    // Functions from other modules: arity and annotated types only.
    Type* fnTypeForeign(FnDecl* f) {
        Type* t = fnType(f);
        if (!f->ret) t->ret = tAny;
        return t;
    }

    Type* fieldType(ClassInfo* ci, int idx) {
        if (idx < (int)ci->fieldTypes.size() && ci->fieldTypes[idx]) return fromTypeExpr(ci->fieldTypes[idx]);
        auto& v = fieldInferred[ci];
        if (idx < (int)v.size() && v[idx]) return v[idx];
        return tAny;
    }

    void inferField(ClassInfo* ci, int idx, Type* t) {
        auto& v = fieldInferred[ci];
        if ((int)v.size() <= idx) v.resize(idx + 1, nullptr);
        v[idx] = join(v[idx], t);
    }

    // ---------------------------------------------------------- suggestions

    static int editDistance(const std::string& a, const std::string& b) {
        std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
        for (size_t i = 1; i <= a.size(); i++) {
            cur[0] = (int)i;
            for (size_t j = 1; j <= b.size(); j++)
                cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
            std::swap(prev, cur);
        }
        return prev[b.size()];
    }
    static std::string suggest(const std::string& name, const std::vector<std::string>& cands) {
        std::string best;
        int bestD = name.size() <= 3 ? 1 : 2;
        for (auto& c : cands) {
            if (c.empty() || c[0] == '$') continue;
            int d = editDistance(name, c);
            if (d <= bestD) {
                bestD = d;
                best = c;
            }
        }
        return best.empty() ? "" : " (did you mean '" + best + "'?)";
    }

    // ---------------------------------------------------------- calls

    std::string calleeName(Expr* callee) {
        if (callee->kind == NK::Ident) return static_cast<Ident*>(callee)->name;
        if (callee->kind == NK::Member) return static_cast<Member*>(callee)->name;
        return "function";
    }

    static std::string arityText(int n) { return std::to_string(n) + (n == 1 ? " argument" : " arguments"); }

    // Parameters (name, has default) of what a function or class type calls, if known.
    bool paramsOf(Type* ct, std::vector<std::pair<std::string, bool>>& out) {
        if (ct->k == Type::Fn && ct->fn) {
            for (size_t i = ct->fn->selfFirst ? 1 : 0; i < ct->fn->params.size(); i++)
                out.push_back({ct->fn->params[i].name, ct->fn->params[i].def != nullptr});
            return true;
        }
        if (ct->k == Type::Class && ct->cls && ct->cls->node) {
            ClassDecl* c = ct->cls->node;
            if (c->hasCtorParams) {
                for (auto& p : c->ctorParams) out.push_back({p.name, p.def != nullptr});
                return true;
            }
            if (ct->cls->init) {
                for (auto& p : ct->cls->init->params) out.push_back({p.name, p.def != nullptr});
                return true;
            }
            return true;  // no parameters
        }
        return false;
    }

    // A call with name: value arguments, matched against the parameters.
    Type* checkNamedCall(Type* ct, const std::vector<Type*>& args, const std::vector<Expr*>& argExprs, SrcPos pos,
                         const std::string& name) {
        std::string nm = ct->k == Type::Class && ct->cls ? ct->cls->name : name;
        std::vector<std::pair<std::string, bool>> params;
        if (!paramsOf(ct, params)) {
            if (ct->k == Type::Fn && ct->arityKnown && !ct->fn)
                err(pos, nm + "() is built in and doesn't take named arguments");
            return ct->k == Type::Fn && ct->ret ? ct->ret : tAny;
        }
        std::vector<Type*> ordered(params.size(), nullptr);
        std::vector<Expr*> orderedExprs(params.size(), nullptr);
        size_t positional = 0;
        for (size_t i = 0; i < argExprs.size(); i++) {
            Expr* a = argExprs[i];
            if (a && a->kind == NK::NamedArg) {
                auto* na = static_cast<NamedArg*>(a);
                size_t k = 0;
                while (k < params.size() && params[k].first != na->name) k++;
                if (k == params.size()) {
                    std::vector<std::string> cands;
                    for (auto& p : params) cands.push_back(p.first);
                    err(na->pos, nm + "() has no parameter '" + na->name + "'" + suggest(na->name, cands),
                        (int)na->name.size());
                    continue;
                }
                if (ordered[k] || k < positional) {
                    err(na->pos, "the argument '" + na->name + "' is given twice", (int)na->name.size());
                    continue;
                }
                ordered[k] = args[i];
                orderedExprs[k] = na;
            } else {
                if (positional < ordered.size()) {
                    ordered[positional] = args[i];
                    orderedExprs[positional] = a;
                } else if (!ct->fn || !ct->fn->lenientArity) {
                    err(pos, nm + "() takes " + arityText((int)params.size()) + " but more were given");
                }
                positional++;
            }
        }
        for (size_t k = 0; k < params.size(); k++)
            if (!ordered[k] && !params[k].second)
                err(pos, nm + "() is missing the argument '" + params[k].first + "'");
        std::unordered_map<std::string, Type*> b;
        for (size_t k = 0; k < params.size() && k < ct->args.size(); k++) {
            if (!ordered[k]) continue;
            bindVars(ct->args[k], ordered[k], b);
            if (!compatible(ordered[k], ct->args[k]))
                err(orderedExprs[k] ? orderedExprs[k]->pos : pos,
                    "argument '" + params[k].first + "' of " + nm + "() should be " + typeToString(ct->args[k]) + ", got " +
                        typeToString(ordered[k]));
        }
        Type* r = ct->ret ? ct->ret : tAny;
        return subst(r, b);
    }

    Type* checkCall(Type* ct, const std::vector<Type*>& args, const std::vector<Expr*>& argExprs, SrcPos pos,
                    const std::string& name) {
        ct = ct ? ct : tAny;
        bool named = false;
        for (Expr* a : argExprs)
            if (a && a->kind == NK::NamedArg) named = true;
        if (named && (ct->k == Type::Fn || ct->k == Type::Class)) return checkNamedCall(ct, args, argExprs, pos, name);
        switch (ct->k) {
            case Type::Any:
            case Type::TypeVar: return tAny;
            case Type::Fn:
            case Type::Class: {
                int n = (int)args.size();
                std::string nm = ct->k == Type::Class && ct->cls ? ct->cls->name : name;
                if (ct->arityKnown && (n < ct->minArgs || (ct->maxArgs >= 0 && n > ct->maxArgs))) {
                    std::string takes;
                    if (ct->maxArgs < 0)
                        takes = "at least " + arityText(ct->minArgs);
                    else if (ct->minArgs == ct->maxArgs)
                        takes = arityText(ct->minArgs);
                    else
                        takes = std::to_string(ct->minArgs) + " to " + arityText(ct->maxArgs);
                    err(pos, nm + "() takes " + takes + " but " + std::to_string(n) + (n == 1 ? " was" : " were") + " given");
                }
                if (ct->fn)
                    for (size_t i = 0; i < argExprs.size(); i++) {
                        size_t pi = i + (ct->fn->selfFirst ? 1 : 0);
                        if (!argExprs[i] || argExprs[i]->kind != NK::Ident || pi >= ct->fn->params.size()) continue;
                        auto* aid = static_cast<Ident*>(argExprs[i]);
                        TypeExpr* pt = ct->fn->params[pi].type.get();
                        if (readOnly(aid->decl) && pt && pt->k == TypeExpr::Ref && pt->mut)
                            borrowError(aid, aid->pos, "it can't be passed on to " + nm + "(), which may change it");
                    }
                std::unordered_map<std::string, Type*> b;
                for (size_t i = 0; i < args.size() && i < ct->args.size(); i++) {
                    bindVars(ct->args[i], args[i], b);
                    if (!compatible(args[i], ct->args[i])) {
                        SrcPos ap = i < argExprs.size() && argExprs[i] ? argExprs[i]->pos : pos;
                        err(ap, "argument " + std::to_string(i + 1) + " of " + nm + "() should be " +
                                    typeToString(ct->args[i]) + ", got " + typeToString(args[i]));
                    }
                }
                Type* r = ct->ret ? ct->ret : tAny;
                return subst(r, b);
            }
            case Type::Namespace: {
                if (ct->name == "vec") return tVec;
                if (ct->name == "mat") return tMat;
                if (ct->name == "quat") return tQuat;
                if (ct->name == "rng") return tRng;
                if (ct->name == "time") return tTime;
                if (ct->name == "Error") return tError;
                err(pos, "'" + ct->name + "' can't be called");
                return tAny;
            }
            case Type::Optional:
            case Type::Result: return checkCall(ct->args[0], args, argExprs, pos, name);
            case Type::Iter:
            case Type::Module: return tAny;
            default: err(pos, "can't call a " + typeToString(ct) + " value"); return tAny;
        }
    }

    static Tag tagFor(Type::K k) {
        switch (k) {
            case Type::Int: return Tag::Int;
            case Type::Float:
            case Type::Num: return Tag::Float;
            case Type::Str: return Tag::Str;
            case Type::List:
            case Type::Iter: return Tag::List;
            case Type::Tuple: return Tag::Tuple;
            case Type::Map: return Tag::Map;
            case Type::Set: return Tag::Set;
            case Type::Range: return Tag::Range;
            case Type::Error: return Tag::Error;
            case Type::Flow: return Tag::Flow;
            case Type::Task: return Tag::Task;
            case Type::Vec: return Tag::Vec;
            case Type::Mat: return Tag::Mat;
            case Type::Quat: return Tag::Quat;
            case Type::Rng: return Tag::Rng;
            case Type::Time: return Tag::Time;
            case Type::Bool: return Tag::Bool;
            case Type::Nil: return Tag::Nil;
            case Type::Fn: return Tag::Fn;
            default: return Tag::COUNT;
        }
    }

    std::vector<std::string> methodNames(Tag tag) {
        std::vector<std::string> out;
        if (tag == Tag::COUNT) return out;
        for (auto& [s, mi] : vm.methods[(int)tag]) out.push_back(vm.symName(s));
        return out;
    }

    // Type of a builtin method (with the receiver's type variables bound), or null.
    Type* builtinMethod(Type* recv, const std::string& name) {
        Tag tag = tagFor(recv->k);
        if (tag == Tag::COUNT) return nullptr;
        auto& table = vm.methods[(int)tag];
        auto it = table.find(vm.sym(name));
        if (it == table.end()) return nullptr;
        Type* t = sigType(it->second.sig);
        if (t->k != Type::Fn) {
            t = mk(Type::Fn);
            t->ret = tAny;
        }
        bool native = isObjType(it->second.fn, OT::Native);
        t = nativeArity(t, it->second.fn, native);
        std::unordered_map<std::string, Type*> b;
        if (recv->k == Type::List || recv->k == Type::Set || recv->k == Type::Flow) b["T"] = recv->args[0];
        if (recv->k == Type::Map) {
            b["K"] = recv->args[0];
            b["V"] = recv->args[1];
        }
        if (recv->k == Type::Range) b["T"] = tInt;
        Type* r = subst(t, b);
        if (r == t) {
            r = mk(Type::Fn);
            *r = *t;
        }
        r->name = typeToString(recv) + "." + name;
        return r;
    }

    // Member access / method call. `args` is null for plain access (auto-call rules apply).
    Type* member(Member* m, Type* recvRaw, const std::vector<Type*>* args, const std::vector<Expr*>& argExprs) {
        Type* recv = strip(recvRaw);
        const std::string& name = m->name;
        int len = (int)name.size();
        auto finish = [&](Type* t, bool isMethod) -> Type* {
            if (!t) return tAny;
            if (args) return checkCall(t, *args, argExprs, m->namePos, t->name.empty() ? name : t->name);
            // obj.name calls a method declared without parentheses (or a built-in
            // method that needs no arguments); &obj.name never calls
            bool calls = t->fn ? fnAutoCalls(t->fn) : t->minArgs == 0;
            if (isMethod && !m->ref && t->k == Type::Fn && calls) return t->ret ? t->ret : tAny;
            return t;
        };
        auto viaUfcs = [&]() -> Type* {
            if (!m->ufcs) return nullptr;
            Type* ft = declType(m->ufcs);
            if (ft->k != Type::Fn && ft->k != Type::Any) return nullptr;
            if (ft->k == Type::Any) return tAny;
            std::vector<Type*> all{recvRaw};
            std::vector<Expr*> allExprs{m->object.get()};
            if (args) {
                all.insert(all.end(), args->begin(), args->end());
                allExprs.insert(allExprs.end(), argExprs.begin(), argExprs.end());
                return checkCall(ft, all, allExprs, m->namePos, name);
            }
            if (m->ref) return tFnAny;
            return checkCall(ft, all, allExprs, m->namePos, name);  // x.f is f(x)
        };
        // (also without parentheses: acc.reset auto-calls it)
        if (recv && !isVague(recv)) {
            Ident* root = rootIdent(m->object.get());
            if (root && readOnly(root->decl)) {
                bool mutates = mutatingBuiltin(recv->k, name);
                if (recv->k == Type::Instance && recv->cls) {
                    auto mi = recv->cls->methods.find(name);
                    if (mi != recv->cls->methods.end() && mutatesSelf(mi->second, recv->cls)) mutates = true;
                }
                if (mutates) borrowError(root, m->namePos, "calling ." + name + " (which changes it) isn't allowed");
            }
        }
        if (!recv || isVague(recv)) return tAny;
        if (recvRaw && recvRaw->k == Type::Optional && recvRaw->declared && m->object->kind == NK::Ident)
            warn(m->object->pos, "'" + static_cast<Ident*>(m->object.get())->name + "' might be nil here; check it first (if " +
                                     static_cast<Ident*>(m->object.get())->name + " == nil -> ...)",
                 (int)static_cast<Ident*>(m->object.get())->name.size());
        // `T ! Error` values: error methods (.kind, .msg, ...) are fine too
        if (recvRaw && (recvRaw->k == Type::Result || recvRaw->k == Type::Error) && recv->k != Type::Error) {
            if (Type* em = builtinMethod(tError, name)) return finish(em, true);
        }
        switch (recv->k) {
            case Type::Instance: {
                ClassInfo* ci = recv->cls;
                if (!ci) return tAny;
                auto fi = ci->fieldIndex.find(name);
                auto targs = typeArgsOf(recv);
                if (fi != ci->fieldIndex.end()) {
                    Type* ft = subst(fieldType(ci, fi->second), targs);
                    if (args) return checkCall(ft, *args, argExprs, m->namePos, name);
                    return ft;
                }
                auto mi = ci->methods.find(name);
                if (mi != ci->methods.end()) {
                    if (!mi->second->exported && ci->module != &mod)
                        err(m->namePos, "method '" + name + "' of " + ci->name +
                                            " is private to its module; mark it with * to make it public (fn " + name + "*)",
                            len);
                    return finish(subst(fnType(mi->second), targs), true);
                }
                if (Type* u = viaUfcs()) return u;
                std::vector<std::string> cands = ci->fields;
                for (auto& [k, v] : ci->methods) cands.push_back(k);
                err(m->namePos, ci->name + " has no field or method '" + name + "'" + suggest(name, cands), len);
                return tAny;
            }
            case Type::Namespace: {
                if (recv->name == "Error") return args ? tError : tError;
                Type* t = nsMember(recv->name, name);
                if (!t) {
                    int idx = vm.builtinIndex(recv->name);
                    std::vector<std::string> cands;
                    if (idx >= 0 && isObjType(vm.builtins[idx].value, OT::Namespace))
                        for (auto& [k, v] : as<ObjNamespace>(vm.builtins[idx].value)->members) cands.push_back(k);
                    err(m->namePos, recv->name + " has no member '" + name + "'" + suggest(name, cands), len);
                    return tAny;
                }
                return finish(t, true);
            }
            case Type::Module: {
                ModuleUnit* mu = recv->module;
                if (!mu) return tAny;
                Decl* d = mu->findExport(name);
                if (!d) {
                    std::string msg = "module '" + mu->name + "' has no exported '" + name + "'";
                    if (mu->globals.count(name)) msg += " (it exists but is private: mark it with *)";
                    err(m->namePos, msg, len);
                    return tAny;
                }
                Type* t = d->kind == DeclKind::Fn && d->node ? fnTypeForeign(static_cast<FnDecl*>(d->node))
                                                            : declType(d);
                return finish(t, true);
            }
            case Type::Class:
                err(m->namePos, "'" + name + "' is read from the class " + (recv->cls ? recv->cls->name : "") +
                                    "; create an instance first", len);
                return tAny;
            case Type::Interface: {
                InterfaceInfo* ii = recv->iface;
                if (!ii) return tAny;
                auto mi = ii->methods.find(name);
                if (mi != ii->methods.end()) return finish(fnType(mi->second), true);
                if (Type* u = viaUfcs()) return u;
                std::vector<std::string> cands;
                for (auto& [k, v] : ii->methods) cands.push_back(k);
                err(m->namePos, "the interface " + ii->name + " has no method '" + name + "'" + suggest(name, cands), len);
                return tAny;
            }
            case Type::Enum: {
                EnumInfo* ei = recv->en;
                if (!ei || !ei->node) return tAny;
                for (auto& fm : ei->node->methods)
                    if (fm->name == name) return finish(fnType(fm.get()), true);
                // a field or method that every variant has
                bool all = !ei->variants.empty();
                for (ClassInfo* v : ei->variants)
                    if (!v || (!v->fieldIndex.count(name) && !v->methods.count(name))) all = false;
                if (all) return tAny;
                if (Type* u = viaUfcs()) return u;
                err(m->namePos, "not every " + ei->name + " has '" + name + "' (match on the variant first)", len);
                return tAny;
            }
            case Type::EnumDef: {
                EnumInfo* ei = recv->en;
                if (!ei) return tAny;
                for (size_t i = 0; i < ei->variantNames.size(); i++) {
                    if (ei->variantNames[i] != name || !ei->variants[i]) continue;
                    ClassInfo* v = ei->variants[i];
                    bool unit = false;
                    for (auto& ev : ei->node->variants)
                        if (ev.name == name) unit = ev.unit;
                    return finish(unit ? instType(v) : classType(v), !unit);
                }
                err(m->namePos, ei->name + " has no variant '" + name + "'" + suggest(name, ei->variantNames), len);
                return tAny;
            }
            default: break;
        }
        if (Type* bm = builtinMethod(recv, name)) return finish(bm, true);
        if (Type* u = viaUfcs()) return u;
        Tag tag = tagFor(recv->k);
        if (tag == Tag::COUNT) return tAny;
        err(m->namePos, typeToString(recv) + " has no method '" + name + "'" + suggest(name, methodNames(tag)), len);
        return tAny;
    }

    // ---------------------------------------------------------- operators

    Type* arith(BinOp op, Type* lRaw, Type* rRaw, SrcPos pos) {
        Type* l = strip(lRaw);
        Type* r = strip(rRaw);
        if (isVague(l)) return tAny;
        if (l->k == Type::Instance || l->k == Type::Interface || l->k == Type::Enum) {
            const char* opName = nullptr;
            switch (op) {
                case BinOp::Add: opName = "+"; break;
                case BinOp::Sub: opName = "-"; break;
                case BinOp::Mul: opName = "*"; break;
                case BinOp::Div: opName = "/"; break;
                case BinOp::Mod: opName = "%"; break;
                case BinOp::Pow: opName = "**"; break;
                default: break;
            }
            if (opName)
                if (FnDecl* f = opMethod(l, opName)) return opResult(f, {rRaw}, pos, opName);
        }
        if (isVague(r)) return tAny;
        auto num = [](Type* t) { return t->k == Type::Int || t->k == Type::Float || t->k == Type::Num; };
        if (num(l) && num(r)) {
            bool ints = l->k == Type::Int && r->k == Type::Int;
            switch (op) {
                case BinOp::Div: return tFloat;
                case BinOp::Root: return tNum;
                case BinOp::BitAnd: case BinOp::BitOr: case BinOp::BitXor: case BinOp::Shl: case BinOp::Shr:
                    if (ints || l->k == Type::Num || r->k == Type::Num) return tInt;
                    break;
                default:
                    if (ints) return tInt;
                    if (l->k == Type::Num || r->k == Type::Num) return tNum;
                    return tFloat;
            }
        }
        auto K = [](Type* t) { return t->k; };
        switch (op) {
            case BinOp::Add:
                if (K(l) == Type::Str && K(r) == Type::Str) return tStr;
                if (K(l) == Type::List && K(r) == Type::List) return listOf(join(l->args[0], r->args[0]));
                if (K(l) == Type::Tuple && K(r) == Type::Tuple) return mk(Type::Tuple);
                if (K(l) == Type::Map && K(r) == Type::Map) return join(l, r);
                if (K(l) == Type::Set && K(r) == Type::Set) return join(l, r);
                if (K(l) == Type::Vec && K(r) == Type::Vec) return tVec;
                if (K(l) == Type::Mat && K(r) == Type::Mat) return tMat;
                if (K(l) == Type::Time && num(r)) return tTime;
                break;
            case BinOp::Sub:
                if (K(l) == Type::Vec && K(r) == Type::Vec) return tVec;
                if (K(l) == Type::Mat && K(r) == Type::Mat) return tMat;
                if (K(l) == Type::Set && K(r) == Type::Set) return l;
                if (K(l) == Type::Time && K(r) == Type::Time) return tFloat;
                if (K(l) == Type::Time && num(r)) return tTime;
                break;
            case BinOp::Mul:
                if ((K(l) == Type::Str && K(r) == Type::Int) || (K(l) == Type::Int && K(r) == Type::Str)) return tStr;
                if (K(l) == Type::List && K(r) == Type::Int) return l;
                if (K(l) == Type::Int && K(r) == Type::List) return r;
                if ((K(l) == Type::Vec && (num(r) || K(r) == Type::Vec)) || (num(l) && K(r) == Type::Vec)) return tVec;
                if (K(l) == Type::Mat && K(r) == Type::Vec) return tVec;
                if ((K(l) == Type::Mat && (K(r) == Type::Mat || num(r))) || (num(l) && K(r) == Type::Mat)) return tMat;
                if (K(l) == Type::Quat && K(r) == Type::Quat) return tQuat;
                if (K(l) == Type::Quat && K(r) == Type::Vec) return tVec;
                break;
            case BinOp::Div:
                if (K(l) == Type::Vec && (num(r) || K(r) == Type::Vec)) return tVec;
                break;
            case BinOp::BitOr:
            case BinOp::BitAnd:
            case BinOp::BitXor:
                if (K(l) == Type::Set && K(r) == Type::Set) return l;
                if (op == BinOp::BitOr && K(l) == Type::Map && K(r) == Type::Map) return join(l, r);
                break;
            default: break;
        }
        std::string msg = std::string("can't use '") + binOpText(op) + "' on " + typeToString(l) + " and " + typeToString(r);
        if (op == BinOp::Add && (K(l) == Type::Str || K(r) == Type::Str)) msg += " (build strings with f\"...{x}\" or x.str)";
        err(pos, msg);
        return tAny;
    }

    bool comparable(Type* l, Type* r) {
        l = strip(l);
        r = strip(r);
        if (isVague(l) || isVague(r)) return true;
        auto num = [](Type* t) { return t->k == Type::Int || t->k == Type::Float || t->k == Type::Num; };
        if (num(l) && num(r)) return true;
        if (opMethod(l, "<")) return true;
        if (l->k != r->k) return false;
        return l->k == Type::Str || l->k == Type::List || l->k == Type::Tuple || l->k == Type::Bool || l->k == Type::Time;
    }

    // ---------------------------------------------------------- expressions

    Type* expr(Expr* e) {
        if (!e) return tNil;
        Type* t = exprInner(e);
        if (!t) t = tAny;
        e->type = t;
        return t;
    }

    Type* exprInner(Expr* e) {
        switch (e->kind) {
            case NK::IntLit: return tInt;
            case NK::NamedArg: return expr(static_cast<NamedArg*>(e)->value.get());
            case NK::FloatLit: return tFloat;
            case NK::StrLit: return tStr;
            case NK::BoolLit: return tBool;
            case NK::NilLit: return tNil;
            case NK::FStr:
                for (auto& p : static_cast<FStr*>(e)->parts) expr(p.expr.get());
                return tStr;
            case NK::Ident: {
                auto* id = static_cast<Ident*>(e);
                if (id->selfMember) return expr(id->selfMember.get());  // self.name
                Decl* d = id->decl;
                if (Type* n = d ? narrowedType(d) : nullptr) return n;
                Type* t = declType(d);
                // fn version = "1": the name alone is a call
                if (!id->callee && !id->ref && t && t->k == Type::Fn && fnAutoCalls(fnDeclOf(d)))
                    return t->ret ? t->ret : tAny;
                return t;
            }
            case NK::Unary: {
                auto* u = static_cast<Unary*>(e);
                Type* t = strip(expr(u->operand.get()));
                if (u->op == UnOp::Not) return tBool;
                if (isVague(t)) return tAny;
                if (u->op == UnOp::Neg) {
                    if (t->k == Type::Int || t->k == Type::Float || t->k == Type::Num || t->k == Type::Vec) return t;
                    if (FnDecl* f = opMethod(t, "neg")) return opResult(f, {}, u->pos, "-");
                    err(u->pos, "can't negate a " + typeToString(t));
                    return tAny;
                }
                if (t->k != Type::Int) err(u->pos, "'~' needs an int, got " + typeToString(t));
                return tInt;
            }
            case NK::Binary: {
                auto* b = static_cast<Binary*>(e);
                Type* l = expr(b->left.get());
                Type* r;
                if (b->op == BinOp::And || b->op == BinOp::Or) {
                    CondFacts f = factsOf(b->left.get());
                    pushFacts(b->op == BinOp::And ? f.yes : f.no);
                    r = expr(b->right.get());
                    narrowed.pop_back();
                } else {
                    r = expr(b->right.get());
                }
                switch (b->op) {
                    case BinOp::And:
                    case BinOp::Or: return join(l, r);
                    case BinOp::Eq:
                    case BinOp::Ne:
                    case BinOp::Is:
                    case BinOp::IsNot: return tBool;
                    case BinOp::In:
                    case BinOp::NotIn: {
                        Type* c = strip(r);
                        if (!isVague(c) && c->k != Type::List && c->k != Type::Tuple && c->k != Type::Str &&
                            c->k != Type::Map && c->k != Type::Set && c->k != Type::Range && c->k != Type::Iter)
                            err(b->pos, "can't use 'in' with a " + typeToString(c));
                        return tBool;
                    }
                    case BinOp::Lt:
                    case BinOp::Le:
                    case BinOp::Gt:
                    case BinOp::Ge:
                        if (!comparable(l, r)) err(b->pos, "can't compare " + typeToString(strip(l)) + " and " + typeToString(strip(r)));
                        return tBool;
                    case BinOp::Send: {
                        Type* c = strip(r);
                        if (!isVague(c) && c->k != Type::Flow) err(b->pos, "':>' sends to a flow, got a " + typeToString(c));
                        return tNil;
                    }
                    default: return arith(b->op, l, r, b->pos);
                }
            }
            case NK::Range: {
                auto* r = static_cast<Range*>(e);
                for (Expr* x : {r->start.get(), r->end.get(), r->step.get()}) {
                    if (!x) continue;
                    Type* t = strip(expr(x));
                    if (!isVague(t) && t->k != Type::Int && t->k != Type::Float && t->k != Type::Num)
                        err(x->pos, "range bounds must be numbers, got " + typeToString(t));
                }
                return tRange;
            }
            case NK::Call: {
                auto* c = static_cast<Call*>(e);
                std::vector<Type*> args;
                std::vector<Expr*> argExprs;
                for (auto& a : c->args) {
                    args.push_back(expr(a.get()));
                    argExprs.push_back(a.get());
                }
                Type* ct = expr(c->callee.get());
                if (!c->typeArgs.empty() && c->callee->kind == NK::Ident) {
                    const std::string& n = static_cast<Ident*>(c->callee.get())->name;
                    Type* a0 = fromTypeExpr(c->typeArgs[0].get());
                    if (n == "flow") return mk1(Type::Flow, a0);
                    if (n == "list") return listOf(a0);
                    if (n == "set") return mk1(Type::Set, a0);
                    if (n == "map") {
                        Type* t = mk(Type::Map);
                        t->args = {a0, c->typeArgs.size() > 1 ? fromTypeExpr(c->typeArgs[1].get()) : tAny};
                        return t;
                    }
                }
                Type* r = checkCall(ct, args, argExprs, c->pos, calleeName(c->callee.get()));
                if (c->callee->kind == NK::Ident && c->args.size() == 1 && c->rparen > c->lparen) {
                    auto* id = static_cast<Ident*>(c->callee.get());
                    if (id->decl && id->decl->kind == DeclKind::Builtin && id->name == "sleep") {
                        // sleep(500) is 500.wait
                        NK k = c->args[0]->kind;
                        bool simple = k == NK::IntLit || k == NK::FloatLit || k == NK::Ident || k == NK::Member ||
                                      k == NK::Call || k == NK::Index;
                        deprecated(c->pos, "sleep(ms) is written ms.wait (500.wait, 2s.wait)",
                                   {{id->start, c->lparen + 1, simple ? "" : "("},
                                    {c->rparen, c->rparen + 1, simple ? ".wait" : ").wait"}});
                    }
                }
                // better types for a few generic builtins
                if (c->callee->kind == NK::Ident) {
                    Ident* id = static_cast<Ident*>(c->callee.get());
                    if (id->decl && id->decl->kind == DeclKind::Builtin && !args.empty()) {
                        if (id->name == "list") return listOf(elemOf(args[0]));
                        if (id->name == "set") return mk1(Type::Set, elemOf(args[0]));
                    }
                }
                return r;
            }
            case NK::Member: {
                auto* m = static_cast<Member*>(e);
                Type* recv = expr(m->object.get());
                if (m->isCall) {
                    std::vector<Type*> args;
                    std::vector<Expr*> argExprs;
                    for (auto& a : m->args) {
                        args.push_back(expr(a.get()));
                        argExprs.push_back(a.get());
                    }
                    return member(m, recv, &args, argExprs);
                }
                return member(m, recv, nullptr, {});
            }
            case NK::Index: {
                auto* x = static_cast<Index*>(e);
                Type* o = strip(expr(x->object.get()));
                Type* i = strip(expr(x->index.get()));
                if (isVague(o)) return tAny;
                bool isRange = i && i->k == Type::Range;
                auto needInt = [&]() {
                    if (!isVague(i) && !isRange && i->k != Type::Int && i->k != Type::Float && i->k != Type::Num)
                        err(x->index->pos, typeToString(o) + " index must be an int, got " + typeToString(i));
                };
                switch (o->k) {
                    case Type::List: needInt(); return isRange ? o : o->args[0];
                    case Type::Tuple:
                        needInt();
                        if (isRange) return mk(Type::Tuple);
                        if (x->index->kind == NK::IntLit) {
                            int64_t k = static_cast<IntLit*>(x->index.get())->value;
                            if (k < 0) k += (int64_t)o->args.size();
                            if (!o->args.empty() && k >= 0 && k < (int64_t)o->args.size()) return o->args[(size_t)k];
                            if (!o->args.empty())
                                err(x->index->pos, "index " + std::to_string(k) + " is out of range for a tuple of length " +
                                                       std::to_string(o->args.size()));
                        }
                        return elemOf(o);
                    case Type::Str: needInt(); return tStr;
                    case Type::Map: return o->args[1];
                    case Type::Range: needInt(); return tInt;
                    case Type::Vec: needInt(); return tFloat;
                    case Type::Mat: return i && i->k == Type::Tuple ? tFloat : tVec;
                    case Type::Iter: return tAny;
                    default:
                        if (FnDecl* f = opMethod(o, "[]")) return opResult(f, {i}, x->pos, "[]");
                        err(x->pos, "can't index a " + typeToString(o));
                        return tAny;
                }
            }
            case NK::ListLit: {
                Type* j = nullptr;
                for (auto& x : static_cast<ListLit*>(e)->elems) j = join(j, expr(x.get()));
                return listOf(j ? j : tAny);
            }
            case NK::SetLit: {
                Type* j = nullptr;
                for (auto& x : static_cast<SetLit*>(e)->elems) j = join(j, expr(x.get()));
                return mk1(Type::Set, j ? j : tAny);
            }
            case NK::TupleLit: {
                Type* t = mk(Type::Tuple);
                for (auto& x : static_cast<TupleLit*>(e)->elems) t->args.push_back(expr(x.get()));
                return t;
            }
            case NK::MapLit: {
                auto* m = static_cast<MapLit*>(e);
                Type *k = nullptr, *v = nullptr;
                for (size_t i = 0; i < m->keys.size(); i++) {
                    Type* kt = expr(m->keys[i].get());
                    Type* st = strip(kt);
                    if (st && (st->k == Type::List || st->k == Type::Map || st->k == Type::Set))
                        err(m->keys[i]->pos, "a " + typeToString(st) + " can't be a map key (use a tuple instead)");
                    k = join(k, kt);
                    v = join(v, expr(m->values[i].get()));
                }
                Type* t = mk(Type::Map);
                t->args = {k ? k : tAny, v ? v : tAny};
                return t;
            }
            case NK::Lambda: {
                FnDecl* f = static_cast<Lambda*>(e)->fn.get();
                fnDecl(f);
                return fnType(f);
            }
            case NK::If: {
                auto* x = static_cast<If*>(e);
                expr(x->cond.get());
                CondFacts f = factsOf(x->cond.get());
                pushFacts(f.yes);
                narrowed.emplace_back();
                Type* a = x->then ? blockInner(x->then.get()) : tNil;
                // what the then-branch leaves behind, for variables the else-path narrows
                Facts thenEnd;
                for (auto& [d, t] : f.no) thenEnd.push_back({d, currentType(d)});
                narrowed.pop_back();
                narrowed.pop_back();
                if (a && !x->otherwise) {
                    // `if x == nil` + `x = "..."`: afterwards x is set either way
                    for (size_t k = 0; k < f.no.size(); k++) {
                        Type* j = join(thenEnd[k].second, f.no[k].second);
                        if (j && !isVague(j) && j->k != Type::Optional && j->k != Type::Nil) narrowed.back()[f.no[k].first] = j;
                    }
                }
                pushFacts(f.no);
                Type* b = x->otherwise ? block(x->otherwise.get()) : tNil;
                narrowed.pop_back();
                // one branch always leaves (return, break, ...): the rest of the block knows more
                if (!a && b) addFacts(f.no);
                if (a && !b && x->otherwise) addFacts(f.yes);
                if (!a && !b) return nullptr;
                return join(a, b);
            }
            case NK::Match: return matchExpr(static_cast<Match*>(e));
            case NK::Catch: {
                auto* c = static_cast<Catch*>(e);
                Type* t = expr(c->expr.get());
                if (c->errDecl) c->errDecl->type = tError;
                Type* h = block(c->handler.get());
                Type* ok = t && t->k == Type::Result ? t->args[0] : (t && t->k == Type::Error ? nullptr : t);
                return join(ok, h);
            }
            case NK::Try: {
                Type* t = expr(static_cast<Try*>(e)->expr.get());
                if (!fns.empty()) fns.back().returned = join(fns.back().returned, tError);  // `?` may return the error
                if (t && t->k == Type::Result) return t->args[0];
                if (t && t->k == Type::Error) return tAny;
                return t;
            }
            case NK::Fork: {
                auto* f = static_cast<Fork*>(e);
                expr(f->call.get());
                return tTask;
            }
            default: return tAny;
        }
    }

    void bindPattern(Pattern* p, Type* t) {
        if (!p) return;
        t = t ? t : tAny;
        switch (p->k) {
            case Pattern::Bind:
                if (p->decl) p->decl->type = t;
                break;
            case Pattern::As: {
                Type* inner = t;
                Pattern* sub = p->subs[0].get();
                if (sub->k == Pattern::TypeP_) inner = namedPatternType(sub->name, t);
                if (sub->k == Pattern::ClassP) {
                    ClassInfo* ci = findClass(sub->name);
                    if (ci) inner = instType(ci);
                }
                bindPattern(sub, t);
                if (p->decl) p->decl->type = inner;
                break;
            }
            case Pattern::TupleP: {
                Type* s = strip(t);
                for (size_t i = 0; i < p->subs.size(); i++) {
                    Type* et = s && s->k == Type::Tuple && i < s->args.size() ? s->args[i] : tAny;
                    bindPattern(p->subs[i].get(), et);
                }
                break;
            }
            case Pattern::MapP: {
                Type* s = strip(t);
                Type* vt = s && s->k == Type::Map && s->args.size() > 1 ? s->args[1] : tAny;
                for (auto& k : p->keys) expr(k.get());
                for (auto& sub : p->subs) bindPattern(sub.get(), vt);
                break;
            }
            case Pattern::ListP: {
                Type* s = strip(t);
                Type* et = s && s->k == Type::List ? s->args[0] : tAny;
                for (auto& sub : p->subs) bindPattern(sub.get(), et);
                if (p->restDecl) p->restDecl->type = listOf(et);
                break;
            }
            case Pattern::ClassP: {
                if (p->value) expr(p->value.get());
                ClassInfo* ci = findClass(p->name);
                Decl* cd = p->value && p->value->kind == NK::Ident ? static_cast<Ident*>(p->value.get())->decl : nullptr;
                if (cd && cd->kind != DeclKind::Class && cd->kind != DeclKind::Import && cd->kind != DeclKind::Builtin &&
                    !((cd->kind == DeclKind::Interface || cd->kind == DeclKind::Enum) && p->subs.empty())) {
                    err(p->pos, "'" + p->name + "' is not a class", (int)p->name.size());
                }
                if (ci && p->inclusive && p->subs.size() > ci->positional.size())
                    err(p->pos, ci->name + " has only " + std::to_string(ci->positional.size()) +
                                    " fields to match against, but the pattern lists " + std::to_string(p->subs.size()));
                for (size_t i = 0; i < p->subs.size(); i++) {
                    Type* ft = ci && i < ci->positional.size() ? fieldType(ci, ci->positional[i]) : tAny;
                    bindPattern(p->subs[i].get(), ft);
                }
                break;
            }
            case Pattern::Alt:
                for (auto& sub : p->subs) bindPattern(sub.get(), t);
                break;
            case Pattern::Literal:
            case Pattern::Value:
            case Pattern::RangeP:
                expr(p->value.get());
                expr(p->value2.get());
                break;
            default: break;
        }
    }

    Type* namedPatternType(const std::string& n, Type* fallback) {
        if (n == "int") return tInt;
        if (n == "float") return tFloat;
        if (n == "num") return tNum;
        if (n == "str") return tStr;
        if (n == "bool") return tBool;
        if (n == "list") return listOf(tAny);
        if (n == "Error") return tError;
        return fallback;
    }

    Type* matchExpr(Match* m) {
        Type* subj = expr(m->subject.get());
        Type* out = nullptr;
        bool exhaustive = false;
        Decl* subjDecl = narrowable(m->subject.get());
        for (auto& arm : m->arms) {
            bindPattern(arm.pat.get(), subj);
            Facts armFacts;
            if (subjDecl) {
                Pattern* p = arm.pat.get();
                if (p->k == Pattern::ClassP) {
                    if (ClassInfo* ci = findClass(p->name)) armFacts.push_back({subjDecl, instType(ci)});
                    else if (Type* t = typeNamed(p->value.get())) armFacts.push_back({subjDecl, t});
                } else if (p->k == Pattern::Value && p->value) {
                    if (Type* t = typeNamed(p->value.get())) armFacts.push_back({subjDecl, t});
                }
            }
            pushFacts(armFacts);
            if (arm.guard) expr(arm.guard.get());
            Type* t = block(arm.body.get());
            narrowed.pop_back();
            out = join(out, t);
            if (!arm.guard && (arm.pat->k == Pattern::Wildcard || arm.pat->k == Pattern::Bind)) exhaustive = true;
        }
        Type* s = strip(subj);
        if (!exhaustive && s && s->k == Type::Enum && s->en) exhaustive = enumCovered(m, s->en);
        if (!exhaustive) out = join(out, tNil);
        return out;
    }

    // Does a match cover every variant of an enum? Warns about the missing ones.
    bool enumCovered(Match* m, EnumInfo* ei) {
        std::unordered_set<std::string> seen;
        bool all = false;
        std::function<void(Pattern*)> visit = [&](Pattern* p) {
            if (p->k == Pattern::Alt) {
                for (auto& sub : p->subs) visit(sub.get());
                return;
            }
            std::string n;
            if (p->k == Pattern::ClassP) {
                bool plain = true;  // Shape.Circle(r) with only bindings covers the variant
                for (auto& sub : p->subs)
                    if (sub->k != Pattern::Bind && sub->k != Pattern::Wildcard) plain = false;
                if (!plain) return;
                n = p->name;
            } else if (p->k == Pattern::Value && p->value && p->value->kind == NK::Member) {
                auto* mm = static_cast<Member*>(p->value.get());
                if (mm->object->kind == NK::Ident) n = static_cast<Ident*>(mm->object.get())->name + "." + mm->name;
            } else if (p->k == Pattern::Value && p->value && p->value->kind == NK::Ident) {
                n = static_cast<Ident*>(p->value.get())->name;
            }
            if (n == ei->name) all = true;
            else if (n.size() > ei->name.size() + 1 && n.compare(0, ei->name.size() + 1, ei->name + ".") == 0)
                seen.insert(n.substr(ei->name.size() + 1));
        };
        for (auto& arm : m->arms)
            if (!arm.guard) visit(arm.pat.get());
        if (all) return true;
        std::string missing;
        for (auto& v : ei->variantNames)
            if (!seen.count(v)) missing += (missing.empty() ? "" : ", ") + ei->name + "." + v;
        if (missing.empty()) return true;
        if (report) {
            std::string key = std::to_string(m->pos.line) + ":" + std::to_string(m->pos.col) + "enum";
            if (reported.insert(key).second)
                diags.warning(mod.path, m->pos, "this match doesn't handle " + missing, 5);
        }
        return false;
    }

    // ---------------------------------------------------------- statements

    // Value type of a block (null when it always leaves via return/break/...).
    Type* block(Block* b) {
        if (!b) return tNil;
        narrowed.emplace_back();
        Type* last = blockInner(b);
        narrowed.pop_back();
        return last;
    }

    // l.sort alone makes a sorted copy and drops it: sort! was meant
    void discardedCopy(Member* m) {
        if (!report || m->name.empty() || m->name.back() == '!' || !m->nameEnd) return;
        Type* recv = m->object ? strip(expr(m->object.get())) : nullptr;
        if (!recv || isVague(recv)) return;
        Tag tag = tagFor(recv->k);
        if (tag == Tag::COUNT) return;
        auto& table = vm.methods[(int)tag];
        if (!table.count(vm.sym(m->name + "!"))) return;
        diags.warning(mod.path, m->namePos,
                      "." + m->name + " makes a new value and it isn't used; ." + m->name + "! changes this one",
                      (int)m->name.size(), {{m->nameEnd, m->nameEnd, "!"}});
    }

    // Error kinds: a match on Error.X (or e.kind == "X") when nothing creates
    // X but something creates a kind spelled almost the same is a typo.
    void checkErrorKinds(Block* program) {
        static const char* const stdKinds[] = {
            "NotFound", "InvalidNumber", "IO", "NetUnavailable", "Http", "Singular", "NotWritable", "NotSquare",
            "InvalidUrl", "InvalidTime", "InvalidJson", "BadSignature", "BadPattern", "AlreadyAnswered",
            "AddressInUse", "MaxRetries"};
        std::unordered_set<std::string> made(std::begin(stdKinds), std::end(stdKinds));
        struct Use {
            std::string kind;
            SrcPos pos;
        };
        std::vector<Use> uses;
        auto errorMember = [](Expr* e, std::string& kind) {
            if (!e || e->kind != NK::Member) return false;
            auto* m = static_cast<Member*>(e);
            if (!m->object || m->object->kind != NK::Ident || static_cast<Ident*>(m->object.get())->name != "Error")
                return false;
            kind = m->name;
            return true;
        };
        std::function<void(Node*)> walk = [&](Node* n) {
            if (!n) return;
            std::string k;
            if (n->kind == NK::Match) {
                auto* mt = static_cast<Match*>(n);
                walk(mt->subject.get());
                std::function<void(Pattern*)> pat = [&](Pattern* p) {
                    if (!p) return;
                    std::string pk;
                    if ((p->k == Pattern::Value || p->k == Pattern::ClassP) && errorMember(p->value.get(), pk))
                        uses.push_back({pk, p->pos});
                    for (auto& s : p->subs) pat(s.get());
                };
                for (auto& arm : mt->arms) {
                    pat(arm.pat.get());
                    walk(arm.guard.get());
                    walk(arm.body.get());
                }
                return;
            }
            if (errorMember(static_cast<Expr*>(n->kind == NK::Member ? n : nullptr), k)) made.insert(k);
            if (n->kind == NK::Binary) {
                auto* b = static_cast<Binary*>(n);
                if (b->op == BinOp::Eq || b->op == BinOp::Ne) {
                    Expr* sides[2] = {b->left.get(), b->right.get()};
                    for (int i = 0; i < 2; i++)
                        if (sides[i]->kind == NK::Member && static_cast<Member*>(sides[i])->name == "kind" &&
                            sides[1 - i]->kind == NK::StrLit)
                            uses.push_back({static_cast<StrLit*>(sides[1 - i])->value, sides[1 - i]->pos});
                }
            }
            if (n->kind == NK::Call) {
                auto* c = static_cast<Call*>(n);
                if (c->callee->kind == NK::Ident && static_cast<Ident*>(c->callee.get())->name == "Error" &&
                    !c->args.empty() && c->args[0]->kind == NK::StrLit)
                    made.insert(static_cast<StrLit*>(c->args[0].get())->value);
            }
            forEachChild(n, walk);
        };
        walk(program);
        for (auto& u : uses) {
            if (made.count(u.kind)) continue;
            std::string best;
            size_t bestD = 3;
            for (auto& m : made) {
                // edit distance, small strings
                const std::string& a = u.kind;
                std::vector<size_t> prev(m.size() + 1), cur(m.size() + 1);
                for (size_t j = 0; j <= m.size(); j++) prev[j] = j;
                for (size_t i = 1; i <= a.size(); i++) {
                    cur[0] = i;
                    for (size_t j = 1; j <= m.size(); j++)
                        cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == m[j - 1] ? 0 : 1)});
                    std::swap(prev, cur);
                }
                if (prev[m.size()] < bestD) {
                    bestD = prev[m.size()];
                    best = m;
                }
            }
            if (!best.empty())
                warn(u.pos, "nothing makes the error kind '" + u.kind + "' (did you mean '" + best + "'?)",
                     (int)u.kind.size());
        }
    }

    Type* blockInner(Block* b) {
        Type* last = tNil;
        for (size_t i = 0; i < b->stmts.size(); i++) {
            Stmt* s = b->stmts[i].get();
            bool isLast = i + 1 == b->stmts.size();
            if (s->kind == NK::ExprStmt) {
                Expr* e = static_cast<ExprStmt*>(s)->expr.get();
                Type* t = expr(e);
                if (!isLast && e->kind == NK::Member) discardedCopy(static_cast<Member*>(e));
                if (e->kind == NK::Ident && static_cast<Ident*>(e)->statementCall && t && t->k == Type::Fn) {
                    auto* id = static_cast<Ident*>(e);
                    if (fnDeclOf(id->decl)) {
                        // a function written with parentheses isn't called by its name alone
                        if (!id->ref)
                            warn(e->pos, "'" + id->name + "' does nothing here: call it with " + id->name + "()",
                                 (int)id->name.size());
                    } else if (t->minArgs == 0) {
                        t = t->ret ? t->ret : tAny;
                    }
                } else if (e->kind == NK::Member && !static_cast<Member*>(e)->isCall && !static_cast<Member*>(e)->ref &&
                           t && t->k == Type::Fn && t->fn && !fnAutoCalls(t->fn)) {
                    auto* m = static_cast<Member*>(e);
                    warn(m->namePos, "'." + m->name + "' does nothing here: call it with ." + m->name + "()",
                         (int)m->name.size());
                }
                if (isLast) last = t;
                continue;
            }
            if (isLast && s->kind == NK::Region) {
                last = block(static_cast<Region*>(s)->body.get());
                if (static_cast<Region*>(s)->init) expr(static_cast<Region*>(s)->init.get());
                continue;
            }
            stmt(s);
            if (isLast) {
                switch (s->kind) {
                    case NK::Return: case NK::Break: case NK::Continue: case NK::Retry: case NK::Todo: last = nullptr; break;
                    default: last = tNil;
                }
            }
        }
        return last;
    }

    void assignTo(Expr* target, Type* vt, SrcPos pos, TypeExpr* annot) {
        if (target->kind == NK::Ident) {
            auto* id = static_cast<Ident*>(target);
            Decl* d = id->decl;
            if (!d) return;
            forget(d);
            // x: str? = "a" makes x a str until it changes again
            if ((d->annot || annot) && vt && !isVague(vt) && vt->k != Type::Optional && vt->k != Type::Nil &&
                (d->kind == DeclKind::Var || d->kind == DeclKind::Param)) {
                Type* declared = annot ? fromTypeExpr(annot) : declType(d);
                if (declared && declared->k == Type::Optional && compatible(vt, declared)) narrowed.back()[d] = vt;
            }
            if (d->annot || annot) {
                Type* declared = annot ? fromTypeExpr(annot) : declType(d);
                if (!compatible(vt, declared))
                    err(pos, "can't assign a " + typeToString(vt) + " to '" + id->name + "' (declared as " +
                                 typeToString(declared) + ")");
                id->type = declared;
                return;
            }
            if (d->kind == DeclKind::Var || d->kind == DeclKind::Hidden || d->kind == DeclKind::Fn) d->type = join(d->type, vt);
            id->type = d->type;
            return;
        }
        if (target->kind == NK::Member || target->kind == NK::Index) {
            Ident* root = rootIdent(target);
            if (root && readOnly(root->decl)) borrowError(root, target->pos, "it can't be changed here");
        }
        if (target->kind == NK::Member) {
            auto* m = static_cast<Member*>(target);
            Type* o = strip(expr(m->object.get()));
            if (!o || isVague(o)) return;
            if (o->k == Type::Instance && o->cls) {
                ClassInfo* ci = o->cls;
                auto fi = ci->fieldIndex.find(m->name);
                if (fi == ci->fieldIndex.end()) {
                    err(m->namePos, ci->name + " has no field '" + m->name + "'" + suggest(m->name, ci->fields) +
                                        "; declare it in the class body (or set self." + m->name + " in init)",
                        (int)m->name.size());
                    return;
                }
                if (fi->second < (int)ci->fieldTypes.size() && ci->fieldTypes[fi->second]) {
                    Type* ft = fromTypeExpr(ci->fieldTypes[fi->second]);
                    if (!compatible(vt, ft))
                        err(pos, "can't assign a " + typeToString(vt) + " to field '" + m->name + "' (declared as " +
                                     typeToString(ft) + ")");
                } else {
                    inferField(ci, fi->second, vt);
                }
                return;
            }
            if (o->k == Type::Namespace || o->k == Type::Module)
                err(m->namePos, "can't assign to '" + m->name + "' of " + typeToString(o), (int)m->name.size());
            else if (o->k != Type::Iter)
                err(m->namePos, "can't set '" + m->name + "' on a " + typeToString(o), (int)m->name.size());
            return;
        }
        if (target->kind == NK::Index) {
            auto* x = static_cast<Index*>(target);
            Type* o = strip(expr(x->object.get()));
            Type* i = expr(x->index.get());
            if (!o || isVague(o)) return;
            if (o->k == Type::Str || o->k == Type::Tuple) err(x->pos, "a " + typeToString(o) + " can't be changed (it's immutable)");
            else if (o->k == Type::Map) {
                Type* si = strip(i);
                if (si && (si->k == Type::List || si->k == Type::Map || si->k == Type::Set))
                    err(x->index->pos, "a " + typeToString(si) + " can't be a map key (use a tuple instead)");
            } else if (o->k != Type::List && o->k != Type::Vec && o->k != Type::Mat && o->k != Type::Iter &&
                       !opMethod(o, "[]="))
                err(x->pos, "can't assign by index to a " + typeToString(o));
        }
    }

    Type* targetType(Expr* target) {
        if (target->kind == NK::Ident) return declType(static_cast<Ident*>(target)->decl);
        return expr(target);
    }

    void stmt(Stmt* s) {
        switch (s->kind) {
            case NK::ExprStmt: expr(static_cast<ExprStmt*>(s)->expr.get()); break;
            case NK::Assign: {
                auto* a = static_cast<Assign*>(s);
                SelfTargetSwap sw;
                sw.in(a->target);
                Type* vt = expr(a->value.get());
                if (a->op != BinOp::None) {
                    Type* cur = targetType(a->target.get());
                    const char* inplace = a->op == BinOp::Add   ? "+="
                                          : a->op == BinOp::Sub ? "-="
                                          : a->op == BinOp::Mul ? "*="
                                          : a->op == BinOp::Div ? "/="
                                          : a->op == BinOp::Mod ? "%="
                                                                : nullptr;
                    if (inplace && opMethod(cur, inplace))
                        vt = cur;  // the object changes itself
                    else
                        vt = arith(a->op, cur, vt, a->pos);
                }
                assignTo(a->target.get(), vt, a->value ? a->value->pos : a->pos, a->annot.get());
                break;
            }
            case NK::Destructure: {
                auto* d = static_cast<Destructure*>(s);
                SelfTargetSwap sw;
                for (auto& t : d->targets) sw.in(t);
                Type* vt = strip(expr(d->value.get()));
                for (size_t i = 0; i < d->targets.size(); i++) {
                    Type* et = tAny;
                    if (vt && vt->k == Type::Tuple && !vt->args.empty()) {
                        if (vt->args.size() != d->targets.size()) {
                            err(d->pos, "expected " + std::to_string(d->targets.size()) + " values to unpack, got " +
                                            std::to_string(vt->args.size()));
                            break;
                        }
                        et = vt->args[i];
                    } else if (vt && vt->k == Type::List) {
                        et = vt->args[0];
                    } else if (vt && !isVague(vt) && vt->k != Type::Tuple) {
                        err(d->pos, "can't unpack a " + typeToString(vt));
                        break;
                    }
                    assignTo(d->targets[i].get(), et, d->pos, nullptr);
                }
                break;
            }
            case NK::VarDecl: {
                auto* v = static_cast<VarDecl*>(s);
                Type* vt = v->init ? expr(v->init.get()) : tNil;
                if (v->decl) {
                    if (v->annot) {
                        Type* declared = fromTypeExpr(v->annot.get());
                        if (v->init && !compatible(vt, declared))
                            err(v->init->pos, "can't assign a " + typeToString(vt) + " to '" + v->name + "' (declared as " +
                                                  typeToString(declared) + ")");
                    } else {
                        v->decl->type = join(v->decl->type, vt);
                    }
                }
                break;
            }
            case NK::IncDec: {
                auto* x = static_cast<IncDec*>(s);
                SelfTargetSwap sw;
                sw.in(x->target);
                if (x->target->kind != NK::Ident) {
                    Ident* root = rootIdent(x->target.get());
                    if (root && readOnly(root->decl)) borrowError(root, x->pos, "it can't be changed here");
                }
                Type* t = strip(targetType(x->target.get()));
                if (t && !isVague(t) && t->k != Type::Int && t->k != Type::Float && t->k != Type::Num)
                    err(x->pos, std::string("can't ") + (x->inc ? "increment" : "decrement") + " a " + typeToString(t));
                if (x->target->kind != NK::Ident) expr(x->target.get());
                break;
            }
            case NK::Block: block(static_cast<Block*>(s)); break;
            case NK::While: {
                auto* w = static_cast<While*>(s);
                expr(w->cond.get());
                CondFacts f = factsOf(w->cond.get());
                pushFacts(f.yes);
                block(w->body.get());
                narrowed.pop_back();
                addFacts(f.no);
                break;
            }
            case NK::For: {
                auto* f = static_cast<For*>(s);
                Type* it = strip(expr(f->iter.get()));
                if (f->step) expr(f->step.get());
                if (it && !isVague(it) &&
                    !(it->k == Type::List || it->k == Type::Tuple || it->k == Type::Str || it->k == Type::Map ||
                      it->k == Type::Set || it->k == Type::Range || it->k == Type::Flow || it->k == Type::Iter))
                    err(f->iter->pos, "can't loop over a " + typeToString(it));
                if (f->vars.size() == 1) {
                    if (f->vars[0].decl) f->vars[0].decl->type = elemOf(it);
                } else {
                    std::vector<Type*> parts(f->vars.size(), tAny);
                    bool keyed = f->vars.size() == 2 && !f->destructure;
                    if (f->unpackItem) {
                        parts[0] = it && it->k == Type::Map ? it->args[0] : tInt;
                        Type* e = strip(it && it->k == Type::Map ? it->args[1] : elemOf(it));
                        if (e && e->k == Type::Tuple && e->args.size() == f->vars.size() - 1)
                            for (size_t k = 1; k < f->vars.size(); k++) parts[k] = e->args[k - 1];
                    } else if (it && it->k == Type::Map && keyed) {
                        parts[0] = it->args[0];
                        parts[1] = it->args[1];
                    } else if (keyed) {
                        // for i, x in list: the position and the item
                        parts[0] = tInt;
                        parts[1] = elemOf(it);
                        Type* e = strip(elemOf(it));
                        if (it && (it->k == Type::Set || it->k == Type::Flow))
                            err(f->iter->pos, "for a, b in x gives positions or keys, and a " + typeToString(it) +
                                                  " has neither; to unpack items, write for (a, b) in x");
                        else if (e && e->k == Type::Tuple && e->args.size() == 2 && f->fixStart < f->fixEnd)
                            deprecated(f->vars[0].pos,
                                       "for a, b in a list of pairs now gives the position and the pair; write for (" +
                                           f->vars[0].name + ", " + f->vars[1].name + ") in ... to unpack each pair",
                                       {{f->fixStart, f->fixStart, "("}, {f->fixEnd, f->fixEnd, ")"}});
                    } else {
                        Type* e = strip(elemOf(it));
                        if (e && e->k == Type::Tuple && e->args.size() == f->vars.size()) parts = e->args;
                    }
                    for (size_t i = 0; i < f->vars.size(); i++)
                        if (f->vars[i].decl) f->vars[i].decl->type = parts[i];
                }
                block(f->body.get());
                break;
            }
            case NK::ForRecv: {
                auto* f = static_cast<ForRecv*>(s);
                for (size_t i = 0; i < f->chans.size(); i++) {
                    Type* c = strip(expr(f->chans[i].get()));
                    if (c && !isVague(c) && c->k != Type::Flow) err(f->chans[i]->pos, "'<:' receives from a flow, got a " + typeToString(c));
                    if (f->vars[i].decl) f->vars[i].decl->type = c && c->k == Type::Flow ? c->args[0] : tAny;
                }
                block(f->body.get());
                break;
            }
            case NK::Return: {
                auto* r = static_cast<Return*>(s);
                Type* vt = r->value ? expr(r->value.get()) : tNil;
                if (!fns.empty()) {
                    FnCtx& ctx = fns.back();
                    if (ctx.declaredRet && !compatible(vt, ctx.declaredRet))
                        err(r->value ? r->value->pos : r->pos, ctx.fn->name + " should return " + typeToString(ctx.declaredRet) +
                                                                  ", but this returns " + typeToString(vt));
                    ctx.returned = join(ctx.returned, vt);
                }
                break;
            }
            case NK::Retry: {
                auto* r = static_cast<Retry*>(s);
                Type* c = strip(expr(r->count.get()));
                if (r->count && c && !isVague(c) && c->k != Type::Int) err(r->count->pos, "retry count must be an int");
                expr(r->delay.get());
                break;
            }
            case NK::FnDecl: fnDecl(static_cast<FnDecl*>(s)); break;
            case NK::ClassDecl: classDecl(static_cast<ClassDecl*>(s)); break;
            case NK::InterfaceDecl:
                for (auto& fm : static_cast<InterfaceDecl*>(s)->methods)
                    if (fm->hasBody) fnDecl(fm.get());
                break;
            case NK::EnumDecl: {
                auto* ed = static_cast<EnumDecl*>(s);
                for (auto& v : ed->variants) classDecl(v.cls.get());
                for (auto& fm : ed->methods) fnDecl(fm.get());
                break;
            }
            case NK::With: block(static_cast<With*>(s)->body.get()); break;
            case NK::Region: {
                auto* r = static_cast<Region*>(s);
                if (r->init) expr(r->init.get());
                block(r->body.get());
                break;
            }
            case NK::Defer: block(static_cast<Defer*>(s)->body.get()); break;
            case NK::Select:
                for (auto& a : static_cast<Select*>(s)->arms) {
                    Type* t = strip(expr(a.expr.get()));
                    if (a.kind == SelectArm::Recv) {
                        if (t && !isVague(t) && t->k != Type::Flow)
                            err(a.expr->pos, "'<:' receives from a flow, got a " + typeToString(t));
                        if (a.decl) a.decl->type = optional(t && t->k == Type::Flow ? t->args[0] : tAny);
                    } else if (a.kind == SelectArm::After) {
                        if (t && !isVague(t) && t->k != Type::Int && t->k != Type::Float && t->k != Type::Num)
                            err(a.expr->pos, "'after' takes a number of milliseconds, got a " + typeToString(t));
                    }
                    block(a.body.get());
                }
                break;
            case NK::Recv: {
                auto* r = static_cast<Recv*>(s);
                Type* c = strip(expr(r->chan.get()));
                if (c && !isVague(c) && c->k != Type::Flow) err(r->chan->pos, "'<:' receives from a flow, got a " + typeToString(c));
                assignTo(r->target.get(), optional(c && c->k == Type::Flow ? c->args[0] : tAny), r->pos, nullptr);
                break;
            }
            default: break;
        }
    }

    void fnDecl(FnDecl* f) {
        for (auto& p : f->params) {
            if (!p.def) continue;
            Type* dt = expr(p.def.get());
            if (p.type && !compatible(dt, fromTypeExpr(p.type.get())))
                err(p.def->pos, "default value of '" + p.name + "' should be " + typeToString(fromTypeExpr(p.type.get())) +
                                    ", got " + typeToString(dt));
        }
        for (auto& p : f->params)
            if (p.type) fromTypeExpr(p.type.get());  // reports unknown type names
        Type* declared = f->ret ? fromTypeExpr(f->ret.get()) : nullptr;
        fns.push_back({f, declared, nullptr});
        if (!f->isLambda) narrowed.push_back({{nullptr, nullptr}});  // a barrier
        struct PopBarrier {
            std::vector<std::unordered_map<Decl*, Type*>>& n;
            bool on;
            ~PopBarrier() {
                if (on) n.pop_back();
            }
        } popBarrier{narrowed, !f->isLambda};
        Type* bt = f->hasBody ? block(f->body.get()) : nullptr;
        FnCtx ctx = fns.back();
        fns.pop_back();
        if (declared && bt && f->hasBody && !f->body->stmts.empty() &&
            f->body->stmts.back()->kind == NK::ExprStmt && !compatible(bt, declared)) {
            Stmt* last = f->body->stmts.back().get();
            err(last->pos, f->name + " should return " + typeToString(declared) + ", but its last expression is " +
                               typeToString(bt));
        }
        Type* ret = join(ctx.returned, bt);
        if (!f->hasBody) ret = declared ? declared : tAny;
        fnRet[f] = declared ? declared : (ret ? ret : tNil);
    }

    void classDecl(ClassDecl* c) {
        ClassInfo* ci = c->info;
        for (auto& p : c->ctorParams) {
            if (p.def) expr(p.def.get());
            if (p.type) fromTypeExpr(p.type.get());
        }
        for (auto& fd : c->fields) {
            Type* t = fd.def ? expr(fd.def.get()) : tNil;
            if (!ci) continue;
            int idx = ci->fieldIndex[fd.name];
            if (fd.type) {
                Type* ft = fromTypeExpr(fd.type.get());
                if (fd.def && !compatible(t, ft))
                    err(fd.def->pos, "field '" + fd.name + "' should be " + typeToString(ft) + ", got " + typeToString(t));
            } else {
                inferField(ci, idx, t);
            }
        }
        if (ci)
            for (auto& p : c->ctorParams)
                if (!p.type) inferField(ci, ci->fieldIndex[p.name], tAny);
        for (auto& m : c->methods) fnDecl(m.get());
        if (ci) checkInterfaceSignatures(c, ci);
    }

    // A class's methods must fit the signatures of the interfaces it declares.
    void checkInterfaceSignatures(ClassDecl* c, ClassInfo* ci) {
        for (InterfaceInfo* ii : ci->interfaces)
            for (auto& [name, ifn] : ii->methods) {
                FnDecl* own = nullptr;
                for (auto& m : c->methods)
                    if (m->name == name) own = m.get();
                if (!own) continue;
                size_t ip = ifn->params.size(), op = own->params.size();
                size_t ownRequired = 0;
                for (auto& p : own->params)
                    if (!p.def) ownRequired++;
                std::string where = c->name + "." + name, theirs = ii->name + "." + name;
                if (ifn->params.empty() && own->params.empty() && ifn->hasParamList != own->hasParamList) {
                    // fn area is called by its name; fn area() only with (): they must agree
                    err(own->namePos,
                        where + " must be written like " + theirs + ": fn " + name + (ifn->hasParamList ? "()" : "") +
                            " (" + (ifn->hasParamList ? "called with ()" : "called by its name alone") + ")",
                        (int)name.size());
                    continue;
                }
                if (ownRequired > ip || op < ip) {
                    err(own->namePos, where + " takes " + arityText((int)op) + " but " + theirs + " takes " + arityText((int)ip),
                        (int)name.size());
                    continue;
                }
                for (size_t i = 0; i < ip; i++) {
                    if (!ifn->params[i].type || !own->params[i].type) continue;
                    Type* want = fromTypeExpr(ifn->params[i].type.get());
                    Type* have = fromTypeExpr(own->params[i].type.get());
                    if (!compatible(want, have))
                        err(own->params[i].pos, "parameter '" + own->params[i].name + "' of " + where + " is " +
                                                    typeToString(have) + ", but " + theirs + " passes " + typeToString(want),
                            (int)own->params[i].name.size());
                }
                if (ifn->ret) {
                    Type* want = fromTypeExpr(ifn->ret.get());
                    Type* have = own->ret ? fromTypeExpr(own->ret.get()) : nullptr;
                    if (!have) {
                        auto r = fnRet.find(own);
                        have = r != fnRet.end() ? r->second : nullptr;
                    }
                    if (have && !compatible(have, want))
                        err(own->namePos, where + " returns " + typeToString(have) + ", but " + theirs + " should return " +
                                              typeToString(want),
                            (int)name.size());
                }
            }
    }

    void run(Block* program) {
        // Two silent inference passes, then one that reports errors.
        for (int pass = 0; pass < 3; pass++) {
            report = pass == 2;
            block(program);
        }
        checkErrorKinds(program);
    }
};

Checker::Checker(Program& prog, ModuleUnit& mod, Diagnostics& diags) : impl(new Impl(prog, mod, diags)) {}
Checker::~Checker() { delete impl; }
void Checker::check(Block* program) { impl->run(program); }

}  // namespace ixo
