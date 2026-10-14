// GPU compute: kernels from `gpu fn`, buffers, launches, and the CPU fallback (see gpu.hpp).
#include "gpu.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "ast.hpp"
#include "builtins.hpp"

namespace ixo {

namespace {

std::vector<std::unique_ptr<GpuKernel>>& kernels() {
    static std::vector<std::unique_ptr<GpuKernel>> k;
    return k;
}
std::mutex kernelsLock;

// ------------------------------------------------------------------ compiling a kernel

struct KernelCompiler {
    GpuKernel& k;
    std::map<Decl*, int> vars;  // variable -> slot
    std::map<Decl*, int> bufs;  // buffer parameter -> parameter index
    std::string err;
    int errLine = 0, errCol = 0;
    int loops = 0;

    explicit KernelCompiler(GpuKernel& kk) : k(kk) {}

    bool fail(const Node* n, const std::string& msg) {
        if (err.empty()) {
            err = msg;
            errLine = n ? n->pos.line : 0;
            errCol = n ? n->pos.col : 0;
        }
        return false;
    }
    static const char* tyName(GTy t) {
        switch (t) {
            case GTy::Int: return "int";
            case GTy::Float: return "float";
            case GTy::Bool: return "bool";
            case GTy::Buffer: return "buffer";
            default: return "nothing";
        }
    }
    std::unique_ptr<GNode> node(GNode::K kind, GTy t, const Node* at) {
        auto n = std::make_unique<GNode>();
        n->k = kind;
        n->t = t;
        if (at) {
            n->line = at->pos.line;
            n->col = at->pos.col;
        }
        return n;
    }
    // an int converted to a float where a float is wanted
    std::unique_ptr<GNode> toFloat(std::unique_ptr<GNode> e) {
        if (e->t == GTy::Float) return e;
        if (e->k == GNode::IntK) {
            e->k = GNode::FloatK;
            e->fval = (double)e->ival;
            e->t = GTy::Float;
            return e;
        }
        auto c = std::make_unique<GNode>();
        c->k = GNode::Math;
        c->op = GM_ToFloat;
        c->t = GTy::Float;
        c->line = e->line;
        c->col = e->col;
        c->kids.push_back(std::move(e));
        return c;
    }
    bool isGpuName(const Expr* e) const { return e->kind == NK::Ident && static_cast<const Ident*>(e)->name == "gpu"; }

    std::unique_ptr<GNode> expr(Expr* e) {
        switch (e->kind) {
            case NK::IntLit: {
                int64_t v = static_cast<IntLit*>(e)->value;
                if (v < INT32_MIN || v > INT32_MAX) return fail(e, "gpu ints are 32-bit: " + std::to_string(v) + " doesn't fit"), nullptr;
                auto n = node(GNode::IntK, GTy::Int, e);
                n->ival = v;
                return n;
            }
            case NK::FloatLit: {
                auto n = node(GNode::FloatK, GTy::Float, e);
                n->fval = static_cast<FloatLit*>(e)->value;
                return n;
            }
            case NK::BoolLit: {
                auto n = node(GNode::BoolK, GTy::Bool, e);
                n->ival = static_cast<BoolLit*>(e)->value ? 1 : 0;
                return n;
            }
            case NK::Ident: {
                auto* id = static_cast<Ident*>(e);
                if (id->decl && vars.count(id->decl)) {
                    int s = vars[id->decl];
                    auto n = node(GNode::Var, k.vars[(size_t)s], e);
                    n->slot = s;
                    return n;
                }
                if (id->decl && bufs.count(id->decl))
                    return fail(e, "a buffer is used by indexing it (" + id->name + "[i]) or by its length (" + id->name + ".len)"), nullptr;
                if (id->name == "gpu") return fail(e, "use gpu.index here"), nullptr;
                return fail(e, "a gpu fn can only use its parameters and its own variables ('" + id->name + "' is neither)"), nullptr;
            }
            case NK::Member: {
                auto* m = static_cast<Member*>(e);
                if (isGpuName(m->object.get())) {
                    if (m->name == "index" && !m->isCall) return node(GNode::ThreadIdx, GTy::Int, e);
                    return fail(e, "gpu." + m->name + " isn't available in a gpu fn (gpu.index is)"), nullptr;
                }
                Expr* o = m->object.get();
                if (o->kind == NK::Ident && static_cast<Ident*>(o)->decl && bufs.count(static_cast<Ident*>(o)->decl)) {
                    if (m->name != "len" || m->isCall) return fail(e, "a buffer in a gpu fn has only .len"), nullptr;
                    auto n = node(GNode::Len, GTy::Int, e);
                    n->slot = bufs[static_cast<Ident*>(o)->decl];
                    return n;
                }
                // x.sqrt, x.abs, x.floor... on numbers
                static const std::map<std::string, GMath> methods = {{"sqrt", GM_Sqrt}, {"abs", GM_Abs},
                                                                     {"floor", GM_Floor}, {"ceil", GM_Ceil},
                                                                     {"float", GM_ToFloat}, {"int", GM_ToInt}};
                auto it = methods.find(m->name);
                if (it == methods.end() || (m->isCall && !m->args.empty()))
                    return fail(e, "." + m->name + " isn't available in a gpu fn"), nullptr;
                auto x = expr(o);
                if (!x) return nullptr;
                return math(it->second, std::move(x), nullptr, e);
            }
            case NK::Index: {
                auto* ix = static_cast<Index*>(e);
                Expr* o = ix->object.get();
                if (!(o->kind == NK::Ident && static_cast<Ident*>(o)->decl && bufs.count(static_cast<Ident*>(o)->decl)))
                    return fail(e, "only buffers can be indexed in a gpu fn"), nullptr;
                int b = bufs[static_cast<Ident*>(o)->decl];
                auto i = expr(ix->index.get());
                if (!i) return nullptr;
                if (i->t != GTy::Int) return fail(ix->index.get(), "a buffer index must be an int"), nullptr;
                auto n = node(GNode::Load, k.params[(size_t)b].elem, e);
                n->slot = b;
                n->kids.push_back(std::move(i));
                return n;
            }
            case NK::Unary: {
                auto* u = static_cast<Unary*>(e);
                auto x = expr(u->operand.get());
                if (!x) return nullptr;
                if (u->op == UnOp::Neg) {
                    if (x->t != GTy::Int && x->t != GTy::Float) return fail(e, "- needs a number"), nullptr;
                    auto n = node(GNode::Neg, x->t, e);
                    n->kids.push_back(std::move(x));
                    return n;
                }
                if (u->op == UnOp::Not) {
                    if (x->t != GTy::Bool) return fail(e, "'not' needs a bool"), nullptr;
                    auto n = node(GNode::Not, GTy::Bool, e);
                    n->kids.push_back(std::move(x));
                    return n;
                }
                return fail(e, "this operator isn't available in a gpu fn"), nullptr;
            }
            case NK::Binary: {
                auto* b = static_cast<Binary*>(e);
                auto l = expr(b->left.get());
                if (!l) return nullptr;
                auto r = expr(b->right.get());
                if (!r) return nullptr;
                return binary(b->op, std::move(l), std::move(r), e);
            }
            case NK::Call: {
                auto* c = static_cast<Call*>(e);
                std::string name;
                if (c->callee->kind == NK::Ident) name = static_cast<Ident*>(c->callee.get())->name;
                else if (c->callee->kind == NK::Member && static_cast<Member*>(c->callee.get())->object->kind == NK::Ident &&
                         static_cast<Ident*>(static_cast<Member*>(c->callee.get())->object.get())->name == "math")
                    name = static_cast<Member*>(c->callee.get())->name;
                static const std::map<std::string, std::pair<GMath, int>> fns = {
                    {"sqrt", {GM_Sqrt, 1}}, {"abs", {GM_Abs, 1}},   {"min", {GM_Min, 2}},   {"max", {GM_Max, 2}},
                    {"floor", {GM_Floor, 1}}, {"ceil", {GM_Ceil, 1}}, {"sin", {GM_Sin, 1}}, {"cos", {GM_Cos, 1}},
                    {"tan", {GM_Tan, 1}},   {"exp", {GM_Exp, 1}},   {"log", {GM_Log, 1}},   {"pow", {GM_Pow, 2}},
                    {"float", {GM_ToFloat, 1}}, {"int", {GM_ToInt, 1}}};
                auto it = fns.find(name);
                if (it == fns.end())
                    return fail(e, name.empty() ? "a gpu fn can only call math functions"
                                                : "a gpu fn can't call " + name + " (math functions only: sqrt, abs, min, max, floor, ceil, sin, cos, tan, exp, log, pow, float, int)"),
                           nullptr;
                if ((int)c->args.size() != it->second.second)
                    return fail(e, name + "() takes " + std::to_string(it->second.second) + " argument" +
                                       (it->second.second == 1 ? "" : "s")),
                           nullptr;
                auto x = expr(c->args[0].get());
                if (!x) return nullptr;
                std::unique_ptr<GNode> y;
                if (it->second.second == 2) {
                    y = expr(c->args[1].get());
                    if (!y) return nullptr;
                }
                return math(it->second.first, std::move(x), std::move(y), e);
            }
            case NK::StrLit: case NK::FStr: return fail(e, "a gpu fn can't use a str"), nullptr;
            case NK::ListLit: return fail(e, "a gpu fn can't make a list (use a buffer)"), nullptr;
            case NK::MapLit: return fail(e, "a gpu fn can't use a map"), nullptr;
            case NK::Lambda: return fail(e, "a gpu fn can't make a closure"), nullptr;
            case NK::NilLit: return fail(e, "a gpu fn can't use nil"), nullptr;
            default: return fail(e, "this isn't available in a gpu fn (numbers, bools, buffers, math and control flow are)"), nullptr;
        }
    }
    std::unique_ptr<GNode> math(GMath op, std::unique_ptr<GNode> x, std::unique_ptr<GNode> y, const Node* at) {
        bool num = x->t == GTy::Int || x->t == GTy::Float;
        if (!num || (y && y->t != GTy::Int && y->t != GTy::Float)) return fail(at, "math in a gpu fn takes numbers"), nullptr;
        GTy t = GTy::Float;
        switch (op) {
            case GM_Abs: t = x->t; break;
            case GM_Min: case GM_Max:
                if (x->t == GTy::Int && y->t == GTy::Int) t = GTy::Int;
                else {
                    x = toFloat(std::move(x));
                    y = toFloat(std::move(y));
                }
                break;
            case GM_ToInt: t = GTy::Int; break;
            case GM_ToFloat: break;
            default:  // float in, float out
                x = toFloat(std::move(x));
                if (y) y = toFloat(std::move(y));
                break;
        }
        if (op == GM_ToFloat) return toFloat(std::move(x));
        if (op == GM_ToInt && x->t == GTy::Int) return x;
        auto n = node(GNode::Math, t, at);
        n->op = op;
        n->kids.push_back(std::move(x));
        if (y) n->kids.push_back(std::move(y));
        return n;
    }
    std::unique_ptr<GNode> binary(BinOp op, std::unique_ptr<GNode> l, std::unique_ptr<GNode> r, const Node* at) {
        auto num = [](GTy t) { return t == GTy::Int || t == GTy::Float; };
        GBin g;
        GTy t;
        switch (op) {
            case BinOp::Add: case BinOp::Sub: case BinOp::Mul:
                if (!num(l->t) || !num(r->t)) return fail(at, std::string(binOpText(op)) + " needs numbers"), nullptr;
                g = op == BinOp::Add ? GB_Add : op == BinOp::Sub ? GB_Sub : GB_Mul;
                t = l->t == GTy::Int && r->t == GTy::Int ? GTy::Int : GTy::Float;
                break;
            case BinOp::Div: case BinOp::Pow:
                if (!num(l->t) || !num(r->t)) return fail(at, std::string(binOpText(op)) + " needs numbers"), nullptr;
                g = op == BinOp::Div ? GB_Div : GB_Pow;
                t = GTy::Float;
                break;
            case BinOp::IntDiv: case BinOp::Mod:
                if (!num(l->t) || !num(r->t)) return fail(at, std::string(binOpText(op)) + " needs numbers"), nullptr;
                g = op == BinOp::IntDiv ? GB_IntDiv : GB_Mod;
                t = l->t == GTy::Int && r->t == GTy::Int ? GTy::Int : GTy::Float;
                break;
            case BinOp::Lt: case BinOp::Le: case BinOp::Gt: case BinOp::Ge:
                if (!num(l->t) || !num(r->t)) return fail(at, "comparisons need numbers"), nullptr;
                g = op == BinOp::Lt ? GB_Lt : op == BinOp::Le ? GB_Le : op == BinOp::Gt ? GB_Gt : GB_Ge;
                t = GTy::Bool;
                break;
            case BinOp::Eq: case BinOp::Ne:
                if (!((num(l->t) && num(r->t)) || (l->t == GTy::Bool && r->t == GTy::Bool)))
                    return fail(at, "== compares two numbers or two bools here"), nullptr;
                g = op == BinOp::Eq ? GB_Eq : GB_Ne;
                t = GTy::Bool;
                break;
            case BinOp::And: case BinOp::Or:
                if (l->t != GTy::Bool || r->t != GTy::Bool) return fail(at, "'and'/'or' need bools"), nullptr;
                g = op == BinOp::And ? GB_And : GB_Or;
                t = GTy::Bool;
                break;
            case BinOp::BitAnd: case BinOp::BitOr: case BinOp::BitXor: case BinOp::Shl: case BinOp::Shr:
                if (l->t != GTy::Int || r->t != GTy::Int) return fail(at, "bit operators need ints"), nullptr;
                g = op == BinOp::BitAnd ? GB_BitAnd : op == BinOp::BitOr ? GB_BitOr : op == BinOp::BitXor ? GB_BitXor
                    : op == BinOp::Shl ? GB_Shl : GB_Shr;
                t = GTy::Int;
                break;
            default: return fail(at, std::string("'") + binOpText(op) + "' isn't available in a gpu fn"), nullptr;
        }
        // mixed numbers: both as floats
        if (num(l->t) && num(r->t) && (l->t != r->t || g == GB_Div || g == GB_Pow)) {
            l = toFloat(std::move(l));
            r = toFloat(std::move(r));
        }
        auto n = node(GNode::Bin, t, at);
        n->op = g;
        n->kids.push_back(std::move(l));
        n->kids.push_back(std::move(r));
        return n;
    }

    // a new or existing variable for decl, holding values of type t
    int varFor(Decl* d, GTy t, const Node* at) {
        auto it = vars.find(d);
        if (it != vars.end()) {
            GTy have = k.vars[(size_t)it->second];
            if (have != t && !(have == GTy::Float && t == GTy::Int)) {
                fail(at, "'" + d->name + "' holds a " + tyName(have) + "; this is a " + tyName(t) +
                             (have == GTy::Int && t == GTy::Float ? " (int(...) converts)" : ""));
                return -1;
            }
            return it->second;
        }
        int s = (int)k.vars.size();
        k.vars.push_back(t);
        vars[d] = s;
        return s;
    }
    std::unique_ptr<GNode> setVar(int s, std::unique_ptr<GNode> v, const Node* at) {
        if (k.vars[(size_t)s] == GTy::Float) v = toFloat(std::move(v));
        auto n = node(GNode::Set, GTy::Void, at);
        n->slot = s;
        n->kids.push_back(std::move(v));
        return n;
    }

    std::unique_ptr<GNode> block(Block* b) {
        auto n = node(GNode::Block, GTy::Void, b);
        if (!b) return n;
        for (auto& st : b->stmts) {
            auto x = stmt(st.get());
            if (!x) return nullptr;
            n->kids.push_back(std::move(x));
        }
        return n;
    }
    std::unique_ptr<GNode> stmt(Stmt* s) {
        switch (s->kind) {
            case NK::ExprStmt: {
                Expr* e = static_cast<ExprStmt*>(s)->expr.get();
                if (e->kind == NK::If) return ifStmt(static_cast<If*>(e));
                if (e->kind == NK::StrLit && static_cast<StrLit*>(e)->docString) return node(GNode::Nop, GTy::Void, s);
                return fail(s, "a statement in a gpu fn must do something (assign, if, while, for)"), nullptr;
            }
            case NK::Assign: {
                auto* a = static_cast<Assign*>(s);
                Expr* target = a->target.get();
                if (a->exported) return fail(s, "a gpu fn has no module variables"), nullptr;
                // x op= v is x = x op v
                auto value = [&]() -> std::unique_ptr<GNode> {
                    auto v = expr(a->value.get());
                    if (!v || a->op == BinOp::None) return v;
                    auto cur = expr(target);
                    if (!cur) return nullptr;
                    return binary(a->op, std::move(cur), std::move(v), s);
                };
                if (target->kind == NK::Ident) {
                    auto* id = static_cast<Ident*>(target);
                    if (!id->decl) return fail(s, "'" + id->name + "' isn't known here"), nullptr;
                    if (bufs.count(id->decl)) return fail(s, "a buffer parameter can't be replaced (write its items: " + id->name + "[i] = ...)"), nullptr;
                    if (!vars.count(id->decl) && (id->decl->isGlobal || id->decl->owner != fnInfo))
                        return fail(s, "a gpu fn can't change variables outside it ('" + id->name + "')"), nullptr;
                    auto v = value();
                    if (!v) return nullptr;
                    if (v->t != GTy::Int && v->t != GTy::Float && v->t != GTy::Bool)
                        return fail(s, "a gpu fn's variables hold ints, floats or bools"), nullptr;
                    int slot = varFor(id->decl, v->t, s);
                    if (slot < 0) return nullptr;
                    return setVar(slot, std::move(v), s);
                }
                if (target->kind == NK::Index) {
                    auto* ix = static_cast<Index*>(target);
                    Expr* o = ix->object.get();
                    if (!(o->kind == NK::Ident && static_cast<Ident*>(o)->decl && bufs.count(static_cast<Ident*>(o)->decl)))
                        return fail(s, "only buffer items can be set in a gpu fn"), nullptr;
                    int b = bufs[static_cast<Ident*>(o)->decl];
                    auto i = expr(ix->index.get());
                    if (!i) return nullptr;
                    if (i->t != GTy::Int) return fail(ix->index.get(), "a buffer index must be an int"), nullptr;
                    auto v = value();
                    if (!v) return nullptr;
                    GTy elem = k.params[(size_t)b].elem;
                    if (elem == GTy::Float && v->t == GTy::Int) v = toFloat(std::move(v));
                    if (v->t != elem)
                        return fail(s, std::string("this buffer holds ") + (elem == GTy::Float ? "floats" : "ints") + "; the value is a " + tyName(v->t) +
                                           (elem == GTy::Int ? " (int(...) converts)" : "")),
                               nullptr;
                    auto n = node(GNode::Store, GTy::Void, s);
                    n->slot = b;
                    n->kids.push_back(std::move(i));
                    n->kids.push_back(std::move(v));
                    return n;
                }
                return fail(s, "a gpu fn can set its variables and buffer items only"), nullptr;
            }
            case NK::VarDecl: {
                auto* v = static_cast<VarDecl*>(s);
                if (!v->init) return fail(s, "a variable in a gpu fn needs a value"), nullptr;
                auto e = expr(v->init.get());
                if (!e) return nullptr;
                int slot = varFor(v->decl, e->t, s);
                if (slot < 0) return nullptr;
                return setVar(slot, std::move(e), s);
            }
            case NK::IncDec: {
                auto* d = static_cast<IncDec*>(s);
                if (d->target->kind != NK::Ident || !static_cast<Ident*>(d->target.get())->decl ||
                    !vars.count(static_cast<Ident*>(d->target.get())->decl))
                    return fail(s, "++/-- work on a gpu fn's own variables"), nullptr;
                int slot = vars[static_cast<Ident*>(d->target.get())->decl];
                auto cur = node(GNode::Var, k.vars[(size_t)slot], s);
                cur->slot = slot;
                auto one = node(GNode::IntK, GTy::Int, s);
                one->ival = 1;
                auto sum = binary(d->inc ? BinOp::Add : BinOp::Sub, std::move(cur), std::move(one), s);
                if (!sum) return nullptr;
                return setVar(slot, std::move(sum), s);
            }
            case NK::Block: return block(static_cast<Block*>(s));
            case NK::While: {
                auto* w = static_cast<While*>(s);
                auto c = expr(w->cond.get());
                if (!c) return nullptr;
                if (c->t != GTy::Bool) return fail(w->cond.get(), "a while condition must be a bool"), nullptr;
                loops++;
                auto body = block(w->body.get());
                loops--;
                if (!body) return nullptr;
                auto n = node(GNode::While, GTy::Void, s);
                n->kids.push_back(std::move(c));
                n->kids.push_back(std::move(body));
                return n;
            }
            case NK::For: return forStmt(static_cast<For*>(s));
            case NK::Break: case NK::Continue:
                if (!loops) return fail(s, "break/continue outside a loop"), nullptr;
                return node(s->kind == NK::Break ? GNode::Break : GNode::Continue, GTy::Void, s);
            case NK::Return:
                if (static_cast<Return*>(s)->value) return fail(s, "a gpu fn returns nothing: it writes its results into buffers"), nullptr;
                return node(GNode::Return, GTy::Void, s);
            case NK::FnDecl: return fail(s, "a gpu fn can't define functions"), nullptr;
            default: return fail(s, "this statement isn't available in a gpu fn"), nullptr;
        }
    }
    std::unique_ptr<GNode> ifStmt(If* f) {
        auto c = expr(f->cond.get());
        if (!c) return nullptr;
        if (c->t != GTy::Bool) return fail(f->cond.get(), "an if condition must be a bool"), nullptr;
        auto then = block(f->then.get());
        if (!then) return nullptr;
        auto els = block(f->otherwise.get());
        if (!els) return nullptr;
        auto n = node(GNode::If, GTy::Void, f);
        n->kids.push_back(std::move(c));
        n->kids.push_back(std::move(then));
        n->kids.push_back(std::move(els));
        return n;
    }
    // for v in a..b (by s): v = a; end = b; while v < end (or >, <=, >=) { body; v += s }
    std::unique_ptr<GNode> forStmt(For* f) {
        if (f->vars.size() != 1 || f->destructure || f->iter->kind != NK::Range)
            return fail(f, "a gpu fn's for loops go over a range: for i in a..b"), nullptr;
        auto* r = static_cast<Range*>(f->iter.get());
        if (!r->start || !r->end) return fail(f, "a gpu fn's ranges need both ends"), nullptr;
        auto a = expr(r->start.get());
        if (!a) return nullptr;
        auto b = expr(r->end.get());
        if (!b) return nullptr;
        if (a->t != GTy::Int || b->t != GTy::Int) return fail(f, "a gpu fn's ranges are of ints"), nullptr;
        int64_t step = 1;
        Expr* stepE = f->step ? f->step.get() : r->step.get();
        if (stepE) {
            bool neg = false;
            Expr* x = stepE;
            if (x->kind == NK::Unary && static_cast<Unary*>(x)->op == UnOp::Neg) {
                neg = true;
                x = static_cast<Unary*>(x)->operand.get();
            }
            if (x->kind != NK::IntLit || static_cast<IntLit*>(x)->value == 0)
                return fail(stepE, "a gpu fn's loop step must be a constant int other than 0"), nullptr;
            step = neg ? -static_cast<IntLit*>(x)->value : static_cast<IntLit*>(x)->value;
        }
        int v = varFor(f->vars[0].decl, GTy::Int, f);
        if (v < 0) return nullptr;
        int end = (int)k.vars.size();
        k.vars.push_back(GTy::Int);
        auto out = node(GNode::Block, GTy::Void, f);
        out->kids.push_back(setVar(v, std::move(a), f));
        out->kids.push_back(setVar(end, std::move(b), f));
        auto cv = node(GNode::Var, GTy::Int, f);
        cv->slot = v;
        auto ce = node(GNode::Var, GTy::Int, f);
        ce->slot = end;
        auto cond = node(GNode::Bin, GTy::Bool, f);
        cond->op = step > 0 ? (r->inclusive ? GB_Le : GB_Lt) : (r->inclusive ? GB_Ge : GB_Gt);
        cond->kids.push_back(std::move(cv));
        cond->kids.push_back(std::move(ce));
        loops++;
        auto body = block(f->body.get());
        loops--;
        if (!body) return nullptr;
        auto inc = node(GNode::IntK, GTy::Int, f);
        inc->ival = step;
        auto cur = node(GNode::Var, GTy::Int, f);
        cur->slot = v;
        auto sum = node(GNode::Bin, GTy::Int, f);
        sum->op = GB_Add;
        sum->kids.push_back(std::move(cur));
        sum->kids.push_back(std::move(inc));
        auto w = node(GNode::While, GTy::Void, f);
        w->kids.push_back(std::move(cond));
        w->kids.push_back(std::move(body));
        w->kids.push_back(setVar(v, std::move(sum), f));  // the step (continue goes here)
        out->kids.push_back(std::move(w));
        return out;
    }

    FuncInfo* fnInfo = nullptr;

    bool compile(FnDecl* fnd) {
        k.name = fnd->name;
        fnInfo = fnd->info;
        if (fnd->ret) return fail(fnd, "a gpu fn returns nothing: it writes its results into buffers");
        for (auto& p : fnd->params) {
            if (!p.type) return fail(fnd, "the parameters of a gpu fn need a type (int, float, bool, buffer[int] or buffer[float]): '" + p.name + "' has none");
            if (p.def) return fail(fnd, "the parameters of a gpu fn can't have default values");
            TypeExpr* t = p.type.get();
            GParam gp{p.name, GTy::Void, GTy::Void};
            if (t->k == TypeExpr::Named && t->args.empty() && (t->name == "int" || t->name == "float" || t->name == "bool")) {
                gp.t = t->name == "int" ? GTy::Int : t->name == "float" ? GTy::Float : GTy::Bool;
            } else if (t->k == TypeExpr::Named && t->name == "buffer" && t->args.size() == 1 &&
                       (t->args[0]->name == "int" || t->args[0]->name == "float")) {
                gp.t = GTy::Buffer;
                gp.elem = t->args[0]->name == "int" ? GTy::Int : GTy::Float;
            } else {
                return fail(fnd, "a gpu fn's parameter '" + p.name + "' is a " + t->str() +
                                     "; it takes int, float, bool, buffer[int] or buffer[float]");
            }
            if (gp.t == GTy::Buffer) {
                bufs[p.decl] = (int)k.params.size();
                k.buffers++;
            } else {
                vars[p.decl] = (int)k.vars.size();
                k.vars.push_back(gp.t);
            }
            k.params.push_back(gp);
        }
        k.body = block(fnd->body.get());
        return k.body != nullptr && err.empty();
    }
};

// ------------------------------------------------------------------ the CPU fallback

union GWord {
    int32_t i;
    float f;
    uint32_t u;
};

// The kernel as register code: each variable is a register, temporaries follow;
// one tight loop runs it for every thread, on every core.
enum COp : uint8_t {
    C_CONST, C_MOV, C_TID, C_LEN, C_LOAD, C_STORE, C_IADD, C_ISUB, C_IMUL, C_FADD, C_FSUB, C_FMUL, C_FDIV, C_IDIV,
    C_IMOD, C_FIDIV, C_FMOD, C_FPOW, C_ILT, C_ILE, C_IGT, C_IGE, C_IEQ, C_INE, C_FLT, C_FLE, C_FGT, C_FGE, C_FEQ,
    C_FNE, C_AND, C_OR, C_XOR, C_SHL, C_SHR, C_NOT, C_INEG, C_FNEG, C_MATH, C_JMP, C_JZ, C_RET
};
struct CInst {
    COp op;
    uint8_t m;  // C_MATH: GMath; C_MATH also: 1 when the operand is an int
    uint8_t m2;
    int32_t d, a, b;
};

struct CpuCode {
    std::vector<CInst> code;
    int regs = 0;
};

struct CpuCompiler {
    const GpuKernel& k;
    CpuCode out;
    int temp = 0;
    std::vector<std::pair<size_t, size_t>> loops;  // (continue target, break patches start)
    std::vector<std::vector<size_t>> breaks, conts;

    explicit CpuCompiler(const GpuKernel& kk) : k(kk) { temp = (int)k.vars.size(); }
    int tmp() {
        int r = temp++;
        out.regs = std::max(out.regs, temp);
        return r;
    }
    size_t emit(COp op, int d = 0, int a = 0, int b = 0, uint8_t m = 0, uint8_t m2 = 0) {
        out.code.push_back(CInst{op, m, m2, d, a, b});
        return out.code.size() - 1;
    }
    int expr(const GNode& n) {
        switch (n.k) {
            case GNode::IntK: case GNode::BoolK: {
                int r = tmp();
                emit(C_CONST, r, (int32_t)n.ival);
                return r;
            }
            case GNode::FloatK: {
                int r = tmp();
                float f = (float)n.fval;
                int32_t bits;
                std::memcpy(&bits, &f, 4);
                emit(C_CONST, r, bits);
                return r;
            }
            case GNode::Var: return n.slot;
            case GNode::ThreadIdx: {
                int r = tmp();
                emit(C_TID, r);
                return r;
            }
            case GNode::Len: {
                int r = tmp();
                emit(C_LEN, r, n.slot);
                return r;
            }
            case GNode::Load: {
                int i = expr(*n.kids[0]);
                int r = tmp();
                emit(C_LOAD, r, n.slot, i);
                return r;
            }
            case GNode::Neg: {
                int x = expr(*n.kids[0]);
                int r = tmp();
                emit(n.t == GTy::Int ? C_INEG : C_FNEG, r, x);
                return r;
            }
            case GNode::Not: {
                int x = expr(*n.kids[0]);
                int r = tmp();
                emit(C_NOT, r, x);
                return r;
            }
            case GNode::Math: {
                int x = expr(*n.kids[0]);
                int y = n.kids.size() > 1 ? expr(*n.kids[1]) : 0;
                int r = tmp();
                emit(C_MATH, r, x, y, (uint8_t)n.op, n.kids[0]->t == GTy::Int ? 1 : 0);
                return r;
            }
            case GNode::Bin: {
                int x = expr(*n.kids[0]);
                int y = expr(*n.kids[1]);
                int r = tmp();
                GTy t = n.kids[0]->t;
                bool f = t == GTy::Float;
                COp op = C_IADD;
                switch (n.op) {
                    case GB_Add: op = f ? C_FADD : C_IADD; break;
                    case GB_Sub: op = f ? C_FSUB : C_ISUB; break;
                    case GB_Mul: op = f ? C_FMUL : C_IMUL; break;
                    case GB_Div: op = C_FDIV; break;
                    case GB_Pow: op = C_FPOW; break;
                    case GB_IntDiv: op = f ? C_FIDIV : C_IDIV; break;
                    case GB_Mod: op = f ? C_FMOD : C_IMOD; break;
                    case GB_Lt: op = f ? C_FLT : C_ILT; break;
                    case GB_Le: op = f ? C_FLE : C_ILE; break;
                    case GB_Gt: op = f ? C_FGT : C_IGT; break;
                    case GB_Ge: op = f ? C_FGE : C_IGE; break;
                    case GB_Eq: op = f ? C_FEQ : C_IEQ; break;  // (bools are 0/1 ints)
                    case GB_Ne: op = f ? C_FNE : C_INE; break;
                    case GB_And: case GB_BitAnd: op = C_AND; break;
                    case GB_Or: case GB_BitOr: op = C_OR; break;
                    case GB_BitXor: op = C_XOR; break;
                    case GB_Shl: op = C_SHL; break;
                    case GB_Shr: op = C_SHR; break;
                }
                emit(op, r, x, y);
                return r;
            }
            default: return 0;
        }
    }
    void stmt(const GNode& n) {
        int saved = temp;
        switch (n.k) {
            case GNode::Set: {
                int v = expr(*n.kids[0]);
                if (v != n.slot) emit(C_MOV, n.slot, v);
                break;
            }
            case GNode::Store: {
                int i = expr(*n.kids[0]);
                int v = expr(*n.kids[1]);
                emit(C_STORE, n.slot, i, v);
                break;
            }
            case GNode::Block:
                for (auto& c : n.kids) stmt(*c);
                break;
            case GNode::If: {
                int c = expr(*n.kids[0]);
                size_t jz = emit(C_JZ, 0, c, 0);
                temp = saved;
                stmt(*n.kids[1]);
                size_t jmp = emit(C_JMP, 0, 0);
                out.code[jz].b = (int32_t)out.code.size();
                stmt(*n.kids[2]);
                out.code[jmp].a = (int32_t)out.code.size();
                break;
            }
            case GNode::While: {
                size_t top = out.code.size();
                int c = expr(*n.kids[0]);
                size_t jz = emit(C_JZ, 0, c, 0);
                temp = saved;
                breaks.push_back({});
                conts.push_back({});
                stmt(*n.kids[1]);
                size_t cont = out.code.size();
                if (n.kids.size() > 2) stmt(*n.kids[2]);
                emit(C_JMP, 0, (int32_t)top);
                size_t end = out.code.size();
                out.code[jz].b = (int32_t)end;
                for (size_t at : breaks.back()) out.code[at].a = (int32_t)end;
                for (size_t at : conts.back()) out.code[at].a = (int32_t)cont;
                breaks.pop_back();
                conts.pop_back();
                break;
            }
            case GNode::Break: breaks.back().push_back(emit(C_JMP, 0, 0)); break;
            case GNode::Continue: conts.back().push_back(emit(C_JMP, 0, 0)); break;
            case GNode::Return: emit(C_RET); break;
            default: break;
        }
        temp = saved;
    }
};

const CpuCode& cpuCodeFor(GpuKernel& k) {
    static std::mutex lock;
    static std::map<const GpuKernel*, std::unique_ptr<CpuCode>> cache;
    std::lock_guard<std::mutex> g(lock);
    auto& c = cache[&k];
    if (!c) {
        CpuCompiler cc(k);
        cc.out.regs = (int)k.vars.size();
        cc.stmt(*k.body);
        cc.emit(C_RET);
        c = std::make_unique<CpuCode>(std::move(cc.out));
    }
    return *c;
}

int32_t floorDiv(int32_t a, int32_t b) {
    if (b == 0) return 0;  // (as on the device: defined as 0)
    if (b == -1) return (int32_t)(0u - (uint32_t)a);
    int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
int32_t floorMod(int32_t a, int32_t b) {
    if (b == 0 || b == -1) return 0;
    int32_t m = a % b;
    if (m != 0 && ((m < 0) != (b < 0))) m += b;
    return m;
}
float mathOp(uint8_t op, bool ints, GWord x, GWord y, GWord& out) {
    switch (op) {
        case GM_Sqrt: out.f = std::sqrt(x.f); break;
        case GM_Abs:
            if (ints) out.i = x.i < 0 ? (int32_t)(0u - x.u) : x.i;
            else out.f = std::fabs(x.f);
            break;
        case GM_Min:
            if (ints) out.i = std::min(x.i, y.i);
            else out.f = std::fmin(x.f, y.f);
            break;
        case GM_Max:
            if (ints) out.i = std::max(x.i, y.i);
            else out.f = std::fmax(x.f, y.f);
            break;
        case GM_Floor: out.f = std::floor(x.f); break;
        case GM_Ceil: out.f = std::ceil(x.f); break;
        case GM_Sin: out.f = std::sin(x.f); break;
        case GM_Cos: out.f = std::cos(x.f); break;
        case GM_Tan: out.f = std::tan(x.f); break;
        case GM_Exp: out.f = std::exp(x.f); break;
        case GM_Log: out.f = std::log(x.f); break;
        case GM_Pow: out.f = std::pow(x.f, y.f); break;
        case GM_ToFloat: out.f = (float)x.i; break;
        case GM_ToInt: {  // toward zero; NaN 0; out of range clamped (as the SPIR-V does it)
            float f = x.f;
            out.i = f != f ? 0 : f >= 2147483648.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f;
            break;
        }
    }
    return 0;
}

struct CpuRun {
    const CpuCode& c;
    const std::vector<ObjGpuBuffer*>& bufs;  // by parameter index (null for scalars)
    const std::vector<GWord>& init;          // the scalar parameters' values
    std::atomic<int64_t>* bad;

    void bounds(int32_t i, int b) {
        int64_t none = -1;
        bad->compare_exchange_strong(none, (int64_t)(uint32_t)i | ((int64_t)b << 32));
    }
    void run(int32_t tid, GWord* r) {
        for (size_t s = 0; s < init.size(); s++) r[s] = init[s];
        const CInst* code = c.code.data();
        for (size_t pc = 0;;) {
            const CInst& in = code[pc++];
            switch (in.op) {
                case C_CONST: r[in.d].i = in.a; break;
                case C_MOV: r[in.d] = r[in.a]; break;
                case C_TID: r[in.d].i = tid; break;
                case C_LEN: r[in.d].i = (int32_t)bufs[(size_t)in.a]->n; break;
                case C_LOAD: {
                    int32_t i = r[in.b].i;
                    ObjGpuBuffer* b = bufs[(size_t)in.a];
                    if ((uint32_t)i >= b->n) {
                        bounds(i, in.a);
                        r[in.d].u = 0;
                    } else {
                        r[in.d].u = static_cast<uint32_t*>(b->data)[i];
                    }
                    break;
                }
                case C_STORE: {
                    int32_t i = r[in.a].i;
                    ObjGpuBuffer* b = bufs[(size_t)in.d];
                    if ((uint32_t)i >= b->n) bounds(i, in.d);
                    else static_cast<uint32_t*>(b->data)[i] = r[in.b].u;
                    break;
                }
                case C_IADD: r[in.d].u = r[in.a].u + r[in.b].u; break;
                case C_ISUB: r[in.d].u = r[in.a].u - r[in.b].u; break;
                case C_IMUL: r[in.d].u = r[in.a].u * r[in.b].u; break;
                case C_FADD: r[in.d].f = r[in.a].f + r[in.b].f; break;
                case C_FSUB: r[in.d].f = r[in.a].f - r[in.b].f; break;
                case C_FMUL: r[in.d].f = r[in.a].f * r[in.b].f; break;
                case C_FDIV: r[in.d].f = r[in.a].f / r[in.b].f; break;
                case C_IDIV: r[in.d].i = floorDiv(r[in.a].i, r[in.b].i); break;
                case C_IMOD: r[in.d].i = floorMod(r[in.a].i, r[in.b].i); break;
                case C_FIDIV: r[in.d].f = std::floor(r[in.a].f / r[in.b].f); break;
                case C_FMOD: {
                    float x = r[in.a].f, y = r[in.b].f;
                    r[in.d].f = x - y * std::floor(x / y);
                    break;
                }
                case C_FPOW: r[in.d].f = std::pow(r[in.a].f, r[in.b].f); break;
                case C_ILT: r[in.d].i = r[in.a].i < r[in.b].i; break;
                case C_ILE: r[in.d].i = r[in.a].i <= r[in.b].i; break;
                case C_IGT: r[in.d].i = r[in.a].i > r[in.b].i; break;
                case C_IGE: r[in.d].i = r[in.a].i >= r[in.b].i; break;
                case C_IEQ: r[in.d].i = r[in.a].i == r[in.b].i; break;
                case C_INE: r[in.d].i = r[in.a].i != r[in.b].i; break;
                case C_FLT: r[in.d].i = r[in.a].f < r[in.b].f; break;
                case C_FLE: r[in.d].i = r[in.a].f <= r[in.b].f; break;
                case C_FGT: r[in.d].i = r[in.a].f > r[in.b].f; break;
                case C_FGE: r[in.d].i = r[in.a].f >= r[in.b].f; break;
                case C_FEQ: r[in.d].i = r[in.a].f == r[in.b].f; break;
                case C_FNE: r[in.d].i = r[in.a].f != r[in.b].f; break;
                case C_AND: r[in.d].u = r[in.a].u & r[in.b].u; break;
                case C_OR: r[in.d].u = r[in.a].u | r[in.b].u; break;
                case C_XOR: r[in.d].u = r[in.a].u ^ r[in.b].u; break;
                case C_SHL: r[in.d].u = r[in.a].u << (r[in.b].u & 31); break;
                case C_SHR: r[in.d].i = r[in.a].i >> (r[in.b].u & 31); break;
                case C_NOT: r[in.d].i = !r[in.a].i; break;
                case C_INEG: r[in.d].u = 0u - r[in.a].u; break;
                case C_FNEG: r[in.d].f = -r[in.a].f; break;
                case C_MATH: {
                    GWord o{};
                    mathOp(in.m, in.m2 != 0, r[in.a], r[in.b], o);
                    r[in.d] = o;
                    break;
                }
                case C_JMP: pc = (size_t)in.a; break;
                case C_JZ:
                    if (!r[in.a].i) pc = (size_t)in.b;
                    break;
                case C_RET: return;
            }
        }
    }
};

void runOnCpu(CpuRun& r, uint32_t n) {
    unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    auto range = [&r](uint32_t lo, uint32_t hi) {
        std::vector<GWord> regs((size_t)std::max(r.c.regs, 1));
        for (uint32_t t = lo; t < hi; t++) r.run((int32_t)t, regs.data());
    };
    if (n < 4096 || cores == 1) {
        range(0, n);
        return;
    }
    std::vector<std::thread> pool;
    uint32_t per = (n + cores - 1) / cores;
    for (unsigned c = 0; c < cores; c++) {
        uint32_t lo = c * per, hi = std::min(n, lo + per);
        if (lo >= hi) break;
        pool.emplace_back(range, lo, hi);
    }
    for (auto& th : pool) th.join();
}

// ------------------------------------------------------------------ devices and buffers

enum class Backend { Unknown, Vulkan, Cpu };
Backend backend = Backend::Unknown;
std::string backendWhy;

Backend currentBackend() {
    if (backend != Backend::Unknown) return backend;
    const char* e = std::getenv("IXO_GPU");
    if (e && std::string(e) == "cpu") {
        backend = Backend::Cpu;
        backendWhy = "IXO_GPU=cpu";
        return backend;
    }
    backend = vkStart(&backendWhy) ? Backend::Vulkan : Backend::Cpu;
    return backend;
}

uint32_t pendingThreads = 0;  // gpu.launch(n, f, ...): the size for the next launch

ObjGpuBuffer* newBuffer(VM& vm, bool floats, uint32_t n) {
    auto* b = vm.alloc<ObjGpuBuffer>();
    b->floats = floats;
    b->n = n;
    size_t bytes = (size_t)std::max<uint32_t>(n, 1) * 4;
    if (currentBackend() == Backend::Vulkan) {
        void* mapped = nullptr;
        b->dev = vkNewBuffer(bytes, &mapped);
        if (b->dev) b->data = mapped;
    }
    if (!b->data) b->data = std::calloc(bytes, 1);
    else std::memset(b->data, 0, bytes);
    if (!b->data) vm.panic("out of memory for a gpu buffer of " + std::to_string(n) + " items");
    vm.grow(b, bytes);
    return b;
}

ObjGpuBuffer* bufferArg(VM& vm, Value v, const char* what) {
    if (!isObjType(v, OT::GpuBuffer)) vm.panic(std::string(what) + " expects a gpu buffer, got a " + vm.typeName(v));
    auto* b = static_cast<ObjGpuBuffer*>(v.o);
    if (!b->data) vm.panic("this gpu buffer was freed");
    return b;
}

void fill(VM& vm, ObjGpuBuffer* b, Value src, const char* what) {
    SeqView items;
    if (!seqOf(src, items)) vm.panic(std::string(what) + " expects a list of numbers, got a " + vm.typeName(src));
    if (items.size() != b->n) vm.panic(std::string(what) + ": the list has " + std::to_string(items.size()) + " items, the buffer " + std::to_string(b->n));
    for (size_t i = 0; i < items.size(); i++) {
        Value x = items[i];
        if (!x.isNum()) vm.panic(std::string(what) + ": item " + std::to_string(i) + " is a " + vm.typeName(x) + ", not a number");
        if (b->floats) {
            b->fl()[i] = (float)x.num();
        } else {
            if (!x.isInt()) vm.panic(std::string(what) + ": item " + std::to_string(i) + " is a float in a buffer of ints");
            if (x.i < INT32_MIN || x.i > INT32_MAX) vm.panic(std::string(what) + ": gpu ints are 32-bit; " + std::to_string(x.i) + " doesn't fit");
            b->in()[i] = (int32_t)x.i;
        }
    }
}

#define NATIVE(name) static Value name([[maybe_unused]] VM& vm, [[maybe_unused]] Value* a, [[maybe_unused]] int n)

// gpu.floats(n or list), gpu.ints(n or list), gpu.buffer(list): ints only if every item is an int
Value makeBuffer(VM& vm, Value* a, bool floats, const char* what) {
    if (a[0].isInt()) {
        if (a[0].i < 0 || a[0].i > INT32_MAX) vm.panic(std::string(what) + ": " + std::to_string(a[0].i) + " isn't a buffer size");
        return Value::object(newBuffer(vm, floats, (uint32_t)a[0].i));
    }
    SeqView items;
    if (!seqOf(a[0], items)) vm.panic(std::string(what) + " expects a size or a list of numbers, got a " + vm.typeName(a[0]));
    if (items.size() > INT32_MAX) vm.panic(std::string(what) + ": too many items");
    Value src = a[0];  // (keeps the list reachable while the buffer is made)
    auto* b = newBuffer(vm, floats, (uint32_t)items.size());
    Value keep = Value::object(b);
    fill(vm, b, src, what);
    return keep;
}
NATIVE(gpu_floats) { return makeBuffer(vm, a, true, "gpu.floats"); }
NATIVE(gpu_ints) { return makeBuffer(vm, a, false, "gpu.ints"); }
NATIVE(gpu_buffer) {
    SeqView items;
    if (!seqOf(a[0], items)) vm.panic("gpu.buffer expects a list of numbers, got a " + vm.typeName(a[0]));
    bool floats = false;
    for (Value x : items) floats |= x.t == VT::Float;
    return makeBuffer(vm, a, floats, "gpu.buffer");
}
NATIVE(gpu_devices) {
    std::vector<Value> out;
    if (currentBackend() == Backend::Vulkan)
        for (auto& name : vkDeviceNames()) out.push_back(vm.vstr(name));
    return vm.newList(std::move(out));
}
NATIVE(gpu_device) {
    if (currentBackend() == Backend::Vulkan) return vm.vstr(vkDeviceName());
    return vm.vstr("cpu");
}
NATIVE(gpu_index) { vm.panic("gpu.index is the thread's position inside a gpu fn"); }
// the shader text of a gpu fn, for WebGPU (and to look at)
static GpuKernel* kernelArg(VM& vm, Value v, const char* what) {
    if (!isObjType(v, OT::Closure) || as<ObjClosure>(v)->fn->gpuKernel < 0) vm.panic(std::string(what) + " expects a gpu fn");
    return gpuKernel(as<ObjClosure>(v)->fn->gpuKernel);
}
NATIVE(gpu_wgsl) {
    GpuKernel* k = kernelArg(vm, a[0], "gpu.wgsl");
    if (k->wgsl.empty()) k->wgsl = wgslFor(*k);
    return vm.vstr(k->wgsl);
}
NATIVE(gpu_spirv) {
    GpuKernel* k = kernelArg(vm, a[0], "gpu.spirv");
    if (k->spirv.empty()) k->spirv = spirvFor(*k);
    std::vector<Value> words;
    for (uint32_t w : k->spirv) words.push_back(Value::integer(w));
    return vm.newList(std::move(words));
}

NATIVE(buf_len) { return Value::integer(bufferArg(vm, a[0], "len")->n); }
NATIVE(buf_list) {
    auto* b = bufferArg(vm, a[0], "list");
    std::vector<Value> out;
    out.reserve(b->n);
    for (uint32_t i = 0; i < b->n; i++) out.push_back(b->floats ? Value::number(b->fl()[i]) : Value::integer(b->in()[i]));
    return vm.newList(std::move(out));
}
NATIVE(buf_upload) {
    fill(vm, bufferArg(vm, a[0], "upload"), a[1], "upload");
    return a[0];
}
NATIVE(buf_free) {
    auto* b = bufferArg(vm, a[0], "free");
    gpuReleaseBuffer(b);
    return Value();
}

// __gpu_launch(kernel id, args...): what a call of a gpu fn runs
NATIVE(gpu_launch) {
    GpuKernel* k = gpuKernel((int)a[0].i);
    if (!k) vm.panic("internal: unknown gpu kernel");
    Value* args = a + 1;
    int argc = n - 1;
    if (argc != (int)k->params.size()) vm.panic(k->name + "() takes " + std::to_string(k->params.size()) + " arguments");
    std::vector<ObjGpuBuffer*> bufs(k->params.size(), nullptr);
    std::vector<GWord> init;
    std::vector<uint32_t> words;
    uint32_t threads = pendingThreads;
    bool sized = pendingThreads > 0;
    pendingThreads = 0;
    for (int i = 0; i < argc; i++) {
        const GParam& p = k->params[(size_t)i];
        Value v = args[i];
        std::string where = "argument '" + p.name + "' of gpu fn " + k->name;
        GWord w{};
        switch (p.t) {
            case GTy::Buffer: {
                if (!isObjType(v, OT::GpuBuffer)) vm.panic(where + " should be a gpu buffer, got a " + vm.typeName(v));
                auto* b = static_cast<ObjGpuBuffer*>(v.o);
                if (!b->data) vm.panic(where + " was freed");
                if (b->floats != (p.elem == GTy::Float))
                    vm.panic(where + std::string(" should be a buffer of ") + (p.elem == GTy::Float ? "floats" : "ints") +
                             (b->floats ? " (gpu.ints makes one)" : " (gpu.floats makes one)"));
                bufs[(size_t)i] = b;
                if (!sized) {  // one thread per item of the first buffer
                    threads = b->n;
                    sized = true;
                }
                continue;
            }
            case GTy::Int:
                if (!v.isInt()) vm.panic(where + " should be an int, got a " + vm.typeName(v));
                if (v.i < INT32_MIN || v.i > INT32_MAX) vm.panic(where + ": gpu ints are 32-bit; " + std::to_string(v.i) + " doesn't fit");
                w.i = (int32_t)v.i;
                break;
            case GTy::Float:
                if (!v.isNum()) vm.panic(where + " should be a float, got a " + vm.typeName(v));
                w.f = (float)v.num();
                break;
            case GTy::Bool:
                if (!v.isBool()) vm.panic(where + " should be a bool, got a " + vm.typeName(v));
                w.i = v.b ? 1 : 0;
                break;
            default: break;
        }
        init.push_back(w);
        words.push_back(w.u);
    }
    if (!sized) vm.panic("gpu fn " + k->name + " has no buffer to size the launch by: use gpu.launch(n, " + k->name + ", ...)");
    int64_t bad = -1;
    bool done = false;
    if (threads > 0 && currentBackend() == Backend::Vulkan) {
        std::vector<VkBufferMem*> devBufs;
        bool allOnDevice = true;
        for (auto* b : bufs)
            if (b) {
                devBufs.push_back(b->dev);
                allOnDevice &= b->dev != nullptr;
            }
        std::string why;
        if (allOnDevice) {
            done = vkLaunch(*k, threads, devBufs, words, bad, &why);
            if (!done && std::getenv("IXO_GPU_LOG")) std::fprintf(stderr, "gpu: %s ran on the CPU (%s)\n", k->name.c_str(), why.c_str());
        }
    }
    if (!done && threads > 0) {
        std::atomic<int64_t> flag{-1};
        CpuRun r{cpuCodeFor(*k), bufs, init, &flag};
        runOnCpu(r, threads);
        bad = flag.load();
    }
    if (bad >= 0) {
        int b = (int)(bad >> 32);
        int32_t idx = (int32_t)(uint32_t)(bad & 0xFFFFFFFF);
        std::string name = b >= 0 && b < (int)k->params.size() ? k->params[(size_t)b].name : "a buffer";
        uint32_t len = b >= 0 && b < (int)bufs.size() && bufs[(size_t)b] ? bufs[(size_t)b]->n : 0;
        vm.panic("gpu fn " + k->name + ": index " + std::to_string(idx) + " is out of range for buffer '" + name +
                 "' of length " + std::to_string(len));
    }
    return Value::object(vm.finishedTask());  // already finished: launches run in order
}
// gpu.launch(n, kernel, args...): n threads
NATIVE(gpu_launch_n) {
    if (!a[0].isInt() || a[0].i < 0 || a[0].i > INT32_MAX) vm.panic("gpu.launch expects a thread count first");
    if (!isObjType(a[1], OT::Closure) || as<ObjClosure>(a[1])->fn->gpuKernel < 0) vm.panic("gpu.launch expects a gpu fn");
    pendingThreads = (uint32_t)a[0].i;
    if (pendingThreads == 0) return Value::object(vm.finishedTask());  // nothing to run
    std::vector<Value> args(a + 2, a + n);
    Value f = a[1];
    return vm.callNested(f, args.data(), (int)args.size());
}

}  // namespace

void gpuReleaseBuffer(ObjGpuBuffer* b) {
    if (!b->data) return;
    if (b->dev) vkFreeBuffer(b->dev);
    else std::free(b->data);
    b->dev = nullptr;
    b->data = nullptr;
}

int gpuCompile(FnDecl* fnd, std::string& err, int& line, int& col) {
    auto k = std::make_unique<GpuKernel>();
    KernelCompiler kc(*k);
    if (!kc.compile(fnd)) {
        err = kc.err.empty() ? "this gpu fn can't be compiled" : kc.err;
        line = kc.errLine;
        col = kc.errCol;
        return -1;
    }
    std::lock_guard<std::mutex> g(kernelsLock);
    kernels().push_back(std::move(k));
    return (int)kernels().size() - 1;
}

GpuKernel* gpuKernel(int id) {
    std::lock_guard<std::mutex> g(kernelsLock);
    return id >= 0 && id < (int)kernels().size() ? kernels()[(size_t)id].get() : nullptr;
}

void registerGpu(VM& vm) {
    vm.defineNative("__gpu_launch", gpu_launch, 1, -1, "fn(int, any...) -> task", "Runs a gpu fn (what calling one does).");
    auto* g = vm.defineNamespace("gpu", "GPU compute: buffers and kernels written as gpu fn.");
    vm.nativeModules.push_back("gpu");
    vm.nsFunc(g, "floats", gpu_floats, 1, 1, "fn(any) -> gpu buffer", "A buffer of 32-bit floats: n zeros, or a list's numbers.");
    vm.nsFunc(g, "ints", gpu_ints, 1, 1, "fn(any) -> gpu buffer", "A buffer of 32-bit ints: n zeros, or a list's ints.");
    vm.nsFunc(g, "buffer", gpu_buffer, 1, 1, "fn(list) -> gpu buffer", "A buffer of a list's numbers: floats if any is a float, else ints.");
    vm.nsFunc(g, "launch", gpu_launch_n, 2, -1, "fn(int, fn, any...) -> task", "Runs a gpu fn over n threads.");
    vm.nsFunc(g, "devices", gpu_devices, 0, 0, "fn() -> list[str]", "The GPUs found (empty: kernels run on the CPU).");
    vm.nsFunc(g, "device", gpu_device, 0, 0, "fn() -> str", "The GPU kernels run on, or \"cpu\".");
    vm.nsFunc(g, "index", gpu_index, 0, 0, "fn() -> int", "Inside a gpu fn: this thread's position (0 to n - 1).");
    vm.nsFunc(g, "wgsl", gpu_wgsl, 1, 1, "fn(fn) -> str", "A gpu fn as WGSL, for WebGPU.");
    vm.nsFunc(g, "spirv", gpu_spirv, 1, 1, "fn(fn) -> list[int]", "A gpu fn as SPIR-V words, for Vulkan.");
    vm.defineMethod(Tag::GpuBuf, "len", buf_len, 0, 0, "fn() -> int", "Number of items.");
    vm.defineMethod(Tag::GpuBuf, "list", buf_list, 0, 0, "fn() -> list", "The items as a list.");
    vm.defineMethod(Tag::GpuBuf, "upload", buf_upload, 1, 1, "fn(list) -> gpu buffer", "Replaces the items with a list's.");
    vm.defineMethod(Tag::GpuBuf, "free", buf_free, 0, 0, "fn()", "Frees the buffer now (the collector would later).");
}

}  // namespace ixo
