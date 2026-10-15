// GPU compute: kernels from `gpu fn`, buffers, launches, and the CPU fallback (see gpu.hpp).
#include "gpu.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "ast.hpp"
#include "builtins.hpp"
#include "gpukern.hpp"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

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
    bool wide = false;  // Ixo's own semantics (64-bit), for maps moved to all cores
    std::map<Decl*, int> sharedOf;  // shared array -> its index in k.shared
    std::map<Decl*, std::pair<int, int>> vecs;  // vec variable -> (first slot, width)
    std::map<Decl*, int> mats;  // mat parameter -> first slot (4x4, row by row)
    int dimW = -1, dimH = -1;   // the launch's width and height (hidden parameters, for gpu.x/y/z)

    explicit KernelCompiler(GpuKernel& kk) : k(kk) {}

    std::unique_ptr<GNode> intK(int64_t v, const Node* at) {
        auto n = node(GNode::IntK, GTy::Int, at);
        n->ival = v;
        return n;
    }
    std::unique_ptr<GNode> varN(int s, const Node* at) {
        auto n = node(GNode::Var, k.vars[(size_t)s], at);
        n->slot = s;
        return n;
    }
    std::unique_ptr<GNode> bin2(GBin op, GTy t, std::unique_ptr<GNode> a, std::unique_ptr<GNode> b, const Node* at) {
        auto n = node(GNode::Bin, t, at);
        n->op = op;
        n->kids.push_back(std::move(a));
        n->kids.push_back(std::move(b));
        return n;
    }
    static bool isGpuCall(Expr* e, const char* name) {
        if (e->kind != NK::Member) return false;
        auto* m = static_cast<Member*>(e);
        return m->isCall && m->name == name && m->object->kind == NK::Ident && static_cast<Ident*>(m->object.get())->name == "gpu";
    }
    Decl* identDecl(Expr* e) { return e->kind == NK::Ident ? static_cast<Ident*>(e)->decl : nullptr; }

    // ---- vec and mat: each component is a float variable (vec2..vec4; mat is 4x4)
    int vwidth(Expr* e) {
        switch (e->kind) {
            case NK::Ident: {
                auto it = vecs.find(identDecl(e));
                return it == vecs.end() ? 0 : it->second.second;
            }
            case NK::Call: {
                auto* c = static_cast<Call*>(e);
                if (c->callee->kind == NK::Ident && static_cast<Ident*>(c->callee.get())->name == "vec") return (int)c->args.size();
                return 0;
            }
            case NK::Binary: {
                auto* b = static_cast<Binary*>(e);
                if (b->op != BinOp::Add && b->op != BinOp::Sub && b->op != BinOp::Mul && b->op != BinOp::Div) return 0;
                int l = mats.count(identDecl(b->left.get())) ? 0 : vwidth(b->left.get());
                return std::max(l, vwidth(b->right.get()));
            }
            case NK::Unary: return static_cast<Unary*>(e)->op == UnOp::Neg ? vwidth(static_cast<Unary*>(e)->operand.get()) : 0;
            case NK::Member: {
                auto* m = static_cast<Member*>(e);
                return m->name == "norm" && !m->isCall ? vwidth(m->object.get()) : 0;
            }
            default: return 0;
        }
    }
    bool vexpr(Expr* e, int w, std::vector<std::unique_ptr<GNode>>& out) {
        out.clear();
        switch (e->kind) {
            case NK::Ident: {
                auto it = vecs.find(identDecl(e));
                if (it == vecs.end() || it->second.second != w) return fail(e, "this vec has " + std::to_string(it == vecs.end() ? 0 : it->second.second) + " components, not " + std::to_string(w));
                for (int i = 0; i < w; i++) out.push_back(varN(it->second.first + i, e));
                return true;
            }
            case NK::Call: {
                auto* c = static_cast<Call*>(e);
                if ((int)c->args.size() != w || w < 2 || w > 4) return fail(e, "a vec in a gpu fn has 2, 3 or 4 components");
                for (auto& a : c->args) {
                    auto x = expr(a.get());
                    if (!x) return false;
                    if (x->t != GTy::Int && x->t != GTy::Float) return fail(a.get(), "a vec's components are numbers");
                    out.push_back(toFloat(std::move(x)));
                }
                return true;
            }
            case NK::Unary: {
                if (!vexpr(static_cast<Unary*>(e)->operand.get(), w, out)) return false;
                for (auto& c : out) {
                    auto n = node(GNode::Neg, GTy::Float, e);
                    n->kids.push_back(std::move(c));
                    c = std::move(n);
                }
                return true;
            }
            case NK::Member: {  // v.norm: v / v.len
                auto* m = static_cast<Member*>(e);
                std::vector<std::unique_ptr<GNode>> v;
                if (!vexpr(m->object.get(), w, v)) return false;
                for (int i = 0; i < w; i++) {
                    auto d2 = dot(m->object.get(), m->object.get(), w, e);
                    if (!d2) return false;
                    auto len = math(GM_Sqrt, std::move(d2), nullptr, e);
                    if (!len) return false;
                    out.push_back(bin2(GB_Div, GTy::Float, std::move(v[(size_t)i]), std::move(len), e));
                }
                return true;
            }
            case NK::Binary: {
                auto* b = static_cast<Binary*>(e);
                Decl* md = identDecl(b->left.get());
                if (b->op == BinOp::Mul && mats.count(md)) {  // m * v (the top-left w x w of m)
                    std::vector<std::unique_ptr<GNode>> v;
                    if (!vexpr(b->right.get(), w, v)) return false;
                    for (int i = 0; i < w; i++) {
                        std::unique_ptr<GNode> sum;
                        for (int j = 0; j < w; j++) {
                            std::vector<std::unique_ptr<GNode>> again;
                            if (!vexpr(b->right.get(), w, again)) return false;
                            auto t = bin2(GB_Mul, GTy::Float, varN(mats[md] + i * 4 + j, e), std::move(again[(size_t)j]), e);
                            sum = sum ? bin2(GB_Add, GTy::Float, std::move(sum), std::move(t), e) : std::move(t);
                        }
                        out.push_back(std::move(sum));
                    }
                    return true;
                }
                int lw = vwidth(b->left.get()), rw = vwidth(b->right.get());
                if (lw && rw && lw != rw) return fail(e, "these vecs have different numbers of components");
                if (rw && !lw && b->op != BinOp::Mul) return fail(e, "a number and a vec combine with * (or the vec comes first)");
                if (b->op == BinOp::Div && rw) return fail(e, "a vec is divided by a number");
                std::vector<std::unique_ptr<GNode>> l, r;
                if (lw && !vexpr(b->left.get(), w, l)) return false;
                if (rw && !vexpr(b->right.get(), w, r)) return false;
                for (int i = 0; i < w; i++) {
                    std::unique_ptr<GNode> x = lw ? std::move(l[(size_t)i]) : expr(b->left.get());
                    std::unique_ptr<GNode> y = rw ? std::move(r[(size_t)i]) : expr(b->right.get());
                    if (!x || !y) return false;
                    auto c = binary(b->op, std::move(x), std::move(y), e);
                    if (!c) return false;
                    out.push_back(toFloat(std::move(c)));
                }
                return true;
            }
            default: return fail(e, "this isn't a vec");
        }
    }
    std::unique_ptr<GNode> dot(Expr* a, Expr* b, int w, const Node* at) {
        std::vector<std::unique_ptr<GNode>> x, y;
        if (!vexpr(a, w, x) || !vexpr(b, w, y)) return nullptr;
        std::unique_ptr<GNode> sum;
        for (int i = 0; i < w; i++) {
            auto t = bin2(GB_Mul, GTy::Float, std::move(x[(size_t)i]), std::move(y[(size_t)i]), at);
            sum = sum ? bin2(GB_Add, GTy::Float, std::move(sum), std::move(t), at) : std::move(t);
        }
        return sum;
    }
    // v = <vec>: the components through temporaries (v = vec(v.y, v.x) swaps)
    std::unique_ptr<GNode> setVec(Decl* d, Expr* value, BinOp op, Expr* target, const Node* at) {
        int w = vwidth(value);
        if (op != BinOp::None) w = std::max(w, vecs.count(d) ? vecs[d].second : 0);
        if (w < 2 || w > 4) return fail(at, "a vec in a gpu fn has 2, 3 or 4 components"), nullptr;
        auto it = vecs.find(d);
        if (it != vecs.end() && it->second.second != w) return fail(at, "'" + d->name + "' is a vec of " + std::to_string(it->second.second)), nullptr;
        if (it == vecs.end()) {
            if (vars.count(d)) return fail(at, "'" + d->name + "' holds a number, not a vec"), nullptr;
            vecs[d] = {(int)k.vars.size(), w};
            for (int i = 0; i < w; i++) k.vars.push_back(GTy::Float);
        }
        std::vector<std::unique_ptr<GNode>> comps;
        if (op != BinOp::None) {
            std::vector<std::unique_ptr<GNode>> cur, add;
            if (!vexpr(target, w, cur)) return nullptr;
            int vw = vwidth(value);
            if (vw && !vexpr(value, w, add)) return nullptr;
            for (int i = 0; i < w; i++) {
                std::unique_ptr<GNode> y = vw ? std::move(add[(size_t)i]) : expr(value);
                if (!y) return nullptr;
                auto c = binary(op, std::move(cur[(size_t)i]), std::move(y), at);
                if (!c) return nullptr;
                comps.push_back(toFloat(std::move(c)));
            }
        } else if (!vexpr(value, w, comps)) {
            return nullptr;
        }
        auto blk = node(GNode::Block, GTy::Void, at);
        int base = vecs[d].first, tmpBase = (int)k.vars.size();
        for (int i = 0; i < w; i++) k.vars.push_back(GTy::Float);
        for (int i = 0; i < w; i++) blk->kids.push_back(setVar(tmpBase + i, std::move(comps[(size_t)i]), at));
        for (int i = 0; i < w; i++) blk->kids.push_back(setVar(base + i, varN(tmpBase + i, at), at));
        return blk;
    }
    static int component(const std::string& n) { return n == "x" ? 0 : n == "y" ? 1 : n == "z" ? 2 : n == "w" ? 3 : -1; }
    static bool uses(Node* n, const std::function<bool(Node*)>& f) {
        bool hit = false;
        std::function<void(Node*)> walk = [&](Node* x) {
            if (!x || hit) return;
            if (f(x)) hit = true;
            forEachChild(x, walk);
        };
        walk(n);
        return hit;
    }

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
                if (id->decl && (vecs.count(id->decl) || mats.count(id->decl)))
                    return fail(e, "'" + id->name + "' is a " + (vecs.count(id->decl) ? "vec" : "mat") + "; a number is needed here"), nullptr;
                if (id->decl && sharedOf.count(id->decl)) return fail(e, "a shared array is used by indexing it"), nullptr;
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
                    if (m->name == "local" && !m->isCall)  // the place in the group of 64
                        return bin2(GB_BitAnd, GTy::Int, node(GNode::ThreadIdx, GTy::Int, e), intK(63, e), e);
                    if (m->name == "group" && !m->isCall)
                        return bin2(GB_Shr, GTy::Int, node(GNode::ThreadIdx, GTy::Int, e), intK(6, e), e);
                    if (m->name == "group_size" && !m->isCall) return intK(64, e);
                    if ((m->name == "x" || m->name == "y" || m->name == "z") && !m->isCall && dimW >= 0) {
                        // a 2D/3D launch: x = i % w, y = (i // w) % h, z = i // (w * h)
                        auto tid = node(GNode::ThreadIdx, GTy::Int, e);
                        if (m->name == "x") return bin2(GB_Mod, GTy::Int, std::move(tid), varN(dimW, e), e);
                        if (m->name == "y")
                            return bin2(GB_Mod, GTy::Int, bin2(GB_IntDiv, GTy::Int, std::move(tid), varN(dimW, e), e), varN(dimH, e), e);
                        return bin2(GB_IntDiv, GTy::Int, std::move(tid), bin2(GB_Mul, GTy::Int, varN(dimW, e), varN(dimH, e), e), e);
                    }
                    return fail(e, "gpu." + m->name + " isn't available in a gpu fn (gpu.index, gpu.x/y/z, gpu.local and gpu.group are)"), nullptr;
                }
                Expr* o = m->object.get();
                if (Decl* od = identDecl(o); od && sharedOf.count(od)) {
                    if (m->name != "len" || m->isCall) return fail(e, "a shared array has [i] and .len"), nullptr;
                    return intK(k.shared[(size_t)sharedOf[od]].second, e);
                }
                if (int vw = vwidth(o); vw > 0) {
                    int c = component(m->name);
                    if (c >= 0 && c < vw && !m->isCall) {
                        std::vector<std::unique_ptr<GNode>> comps;
                        if (!vexpr(o, vw, comps)) return nullptr;
                        return std::move(comps[(size_t)c]);
                    }
                    if (m->name == "len" && !m->isCall) {
                        auto d2 = dot(o, o, vw, e);
                        if (!d2) return nullptr;
                        return math(GM_Sqrt, std::move(d2), nullptr, e);
                    }
                    if (m->name == "dot" && m->isCall && m->args.size() == 1) {
                        if (vwidth(m->args[0].get()) != vw) return fail(e, "dot takes a vec of the same width"), nullptr;
                        return dot(o, m->args[0].get(), vw, e);
                    }
                    return fail(e, "a vec in a gpu fn has .x .y .z .w, .len, .norm and .dot(v)"), nullptr;
                }
                if (o->kind == NK::Ident && static_cast<Ident*>(o)->decl && bufs.count(static_cast<Ident*>(o)->decl)) {
                    if (m->name != "len" || m->isCall) return fail(e, "a buffer in a gpu fn has only .len"), nullptr;
                    auto n = node(GNode::Len, GTy::Int, e);
                    n->slot = bufs[static_cast<Ident*>(o)->decl];
                    return n;
                }
                // x.sqrt, x.abs, x.floor... on numbers
                static const std::map<std::string, GMath> methods = {
                    {"sqrt", GM_Sqrt}, {"abs", GM_Abs},  {"floor", GM_Floor}, {"ceil", GM_Ceil}, {"float", GM_ToFloat},
                    {"int", GM_ToInt}, {"sin", GM_Sin},  {"cos", GM_Cos},     {"tan", GM_Tan},   {"exp", GM_Exp},
                    {"ln", GM_Log}};
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
                if (Decl* od = identDecl(o); od && sharedOf.count(od)) {
                    auto i = expr(ix->index.get());
                    if (!i) return nullptr;
                    if (i->t != GTy::Int) return fail(ix->index.get(), "an index must be an int"), nullptr;
                    int s = sharedOf[od];
                    auto n = node(GNode::SLoad, k.shared[(size_t)s].first, e);
                    n->slot = s;
                    n->kids.push_back(std::move(i));
                    return n;
                }
                if (Decl* od = identDecl(o); od && mats.count(od)) {  // m[i, j] with constant i and j
                    auto* t = ix->index->kind == NK::TupleLit ? static_cast<TupleLit*>(ix->index.get()) : nullptr;
                    if (!t || t->elems.size() != 2 || t->elems[0]->kind != NK::IntLit || t->elems[1]->kind != NK::IntLit)
                        return fail(e, "a mat in a gpu fn is read as m[i, j] with constant i and j"), nullptr;
                    int64_t r = static_cast<IntLit*>(t->elems[0].get())->value, c = static_cast<IntLit*>(t->elems[1].get())->value;
                    if (r < 0 || r > 3 || c < 0 || c > 3) return fail(e, "a mat in a gpu fn is 4x4"), nullptr;
                    return varN(mats[od] + (int)(r * 4 + c), e);
                }
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
        if (wide && (op == GM_Min || op == GM_Max) && x->t != y->t)
            return fail(at, "min and max of an int and a float keep the one they pick as it is: not in a parallel map"), nullptr;
        if (wide && (op == GM_Floor || op == GM_Ceil)) {  // Ixo's floor and ceil give ints
            auto n = node(GNode::Math, GTy::Int, at);
            n->op = op;
            n->kids.push_back(toFloat(std::move(x)));
            return n;
        }
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
        bool intsDivided = false;
        if (wide) {  // where Ixo's own arithmetic differs from the device's
            if (g == GB_Pow && l->t == GTy::Int && r->t == GTy::Int)
                return fail(at, "an int to an int power stays an int in Ixo: not in a parallel map"), nullptr;
            if (g == GB_Div && l->t == GTy::Int && r->t == GTy::Int) intsDivided = true;
        }
        // mixed numbers: both as floats
        if (num(l->t) && num(r->t) && (l->t != r->t || g == GB_Div || g == GB_Pow)) {
            l = toFloat(std::move(l));
            r = toFloat(std::move(r));
        }
        auto n = node(GNode::Bin, t, at);
        n->op = g;
        if (intsDivided) n->ival = 1;  // dividing by an int 0 stops the loop: the map hands it back
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
                if (isGpuCall(e, "barrier")) {  // every thread of the group gets here before any goes on
                    k.groups = true;
                    return node(GNode::Barrier, GTy::Void, s);
                }
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
                if (target->kind == NK::Ident && a->op == BinOp::None &&
                    (isGpuCall(a->value.get(), "shared_floats") || isGpuCall(a->value.get(), "shared_ints"))) {
                    // tile = gpu.shared_floats(64): an array each group of 64 threads shares
                    auto* id = static_cast<Ident*>(target);
                    auto* call = static_cast<Member*>(a->value.get());
                    if (!id->decl || sharedOf.count(id->decl) || vars.count(id->decl))
                        return fail(s, "a shared array is made once, under its own name"), nullptr;
                    if (call->args.size() != 1 || call->args[0]->kind != NK::IntLit || static_cast<IntLit*>(call->args[0].get())->value < 1 ||
                        static_cast<IntLit*>(call->args[0].get())->value > 8192)
                        return fail(s, "a shared array's length is a constant from 1 to 8192"), nullptr;
                    sharedOf[id->decl] = (int)k.shared.size();
                    k.shared.push_back({call->name == "shared_floats" ? GTy::Float : GTy::Int,
                                        (uint32_t)static_cast<IntLit*>(call->args[0].get())->value});
                    k.groups = true;
                    return node(GNode::Nop, GTy::Void, s);
                }
                if (target->kind == NK::Ident && static_cast<Ident*>(target)->decl &&
                    (vecs.count(static_cast<Ident*>(target)->decl) || vwidth(a->value.get()) > 0))
                    return setVec(static_cast<Ident*>(target)->decl, a->value.get(), a->op, target, s);
                if (target->kind == NK::Member) {  // v.x = 1.0
                    auto* m = static_cast<Member*>(target);
                    Decl* vd = identDecl(m->object.get());
                    int c = component(m->name);
                    if (vd && vecs.count(vd) && c >= 0 && c < vecs[vd].second) {
                        auto v = value();
                        if (!v) return nullptr;
                        if (v->t != GTy::Int && v->t != GTy::Float) return fail(s, "a vec's components are numbers"), nullptr;
                        return setVar(vecs[vd].first + c, toFloat(std::move(v)), s);
                    }
                }
                if (target->kind == NK::Index) {
                    auto* ix = static_cast<Index*>(target);
                    if (Decl* od = identDecl(ix->object.get()); od && sharedOf.count(od)) {
                        auto i = expr(ix->index.get());
                        if (!i) return nullptr;
                        if (i->t != GTy::Int) return fail(ix->index.get(), "an index must be an int"), nullptr;
                        auto v = value();
                        if (!v) return nullptr;
                        int sh = sharedOf[od];
                        GTy elem = k.shared[(size_t)sh].first;
                        if (elem == GTy::Float && v->t == GTy::Int) v = toFloat(std::move(v));
                        if (v->t != elem) return fail(s, std::string("this shared array holds ") + tyName(elem) + "s"), nullptr;
                        auto n = node(GNode::SStore, GTy::Void, s);
                        n->slot = sh;
                        n->kids.push_back(std::move(i));
                        n->kids.push_back(std::move(v));
                        return n;
                    }
                }
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
        k.unchecked = std::find(fnd->attrs.begin(), fnd->attrs.end(), "$unsafe") != fnd->attrs.end();
        if (fnd->ret) return fail(fnd, "a gpu fn returns nothing: it writes its results into buffers");
        for (auto& p : fnd->params) {
            if (!p.type) return fail(fnd, "the parameters of a gpu fn need a type (int, float, bool, buffer[int] or buffer[float]): '" + p.name + "' has none");
            if (p.def) return fail(fnd, "the parameters of a gpu fn can't have default values");
            TypeExpr* t = p.type.get();
            int vw = t->k == TypeExpr::Named && t->args.empty()
                         ? (t->name == "vec2" ? 2 : t->name == "vec3" || t->name == "vec" ? 3 : t->name == "vec4" ? 4 : t->name == "mat" ? 16 : 0)
                         : 0;
            if (vw) {  // a vec or mat: one float parameter per component
                k.args.push_back({p.name, vw == 16 ? 2 : 1, vw});
                if (vw == 16) mats[p.decl] = (int)k.vars.size();
                else vecs[p.decl] = {(int)k.vars.size(), vw};
                for (int i = 0; i < vw; i++) {
                    k.vars.push_back(GTy::Float);
                    k.params.push_back({p.name + "." + std::to_string(i), GTy::Float, GTy::Void});
                }
                continue;
            }
            k.args.push_back({p.name, 0, 1});
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
        // gpu.x/y/z: the launch's width and height come as two more int parameters
        if (fnd->body && uses(fnd->body.get(), [](Node* x) {
                if (x->kind != NK::Member) return false;
                auto* m = static_cast<Member*>(x);
                return (m->name == "x" || m->name == "y" || m->name == "z") && m->object->kind == NK::Ident &&
                       static_cast<Ident*>(m->object.get())->name == "gpu";
            })) {
            k.dims = true;
            dimW = (int)k.vars.size();
            k.vars.push_back(GTy::Int);
            k.params.push_back({"$width", GTy::Int, GTy::Void});
            dimH = (int)k.vars.size();
            k.vars.push_back(GTy::Int);
            k.params.push_back({"$height", GTy::Int, GTy::Void});
        }
        k.body = block(fnd->body.get());
        return k.body != nullptr && err.empty();
    }

    // gpu xs.map(fn(v) = e): i = gpu.index; if i < dst.len { v = src[i]; dst[i] = e }
    bool compileMap(FnDecl* lam, const std::vector<std::pair<Decl*, GTy>>& caps, GTy elem) {
        k.name = "map";
        k.wide = wide;
        if (lam->params.size() != 1 || !lam->params[0].decl)
            return fail(lam, "a gpu map takes a lambda with one parameter: gpu xs.map(fn(v) = v * v)");
        Block* b = lam->body.get();
        if (!b || b->stmts.size() != 1 || b->stmts[0]->kind != NK::ExprStmt)
            return fail(lam, "a gpu map's lambda is one expression: gpu xs.map(fn(v) = v * v)");
        k.params.push_back({"items", GTy::Buffer, elem});
        k.params.push_back({"out", GTy::Buffer, GTy::Float});
        k.buffers = 2;
        for (auto& [d, t] : caps) {
            vars[d] = (int)k.vars.size();
            k.vars.push_back(t);
            k.params.push_back({d ? std::string("captured") : std::string(), t, GTy::Void});
        }
        int iv = (int)k.vars.size();
        k.vars.push_back(GTy::Int);
        int vv = (int)k.vars.size();
        k.vars.push_back(elem);
        vars[lam->params[0].decl] = vv;
        auto e = expr(static_cast<ExprStmt*>(b->stmts[0].get())->expr.get());
        if (!e || !err.empty()) return false;
        if (wide && e->t == GTy::Bool) {  // (a parallel map) bools come back as 0 and 1, which they are underneath
            k.outBool = true;
            e->t = GTy::Int;
        }
        if (e->t != GTy::Int && e->t != GTy::Float) return fail(lam, "a gpu map's lambda gives a number (int or float)");
        k.params[1].elem = e->t;
        auto var = [&](int s, GTy t) {
            auto n = node(GNode::Var, t, lam);
            n->slot = s;
            return n;
        };
        auto body = node(GNode::Block, GTy::Void, lam);
        body->kids.push_back(setVar(iv, node(GNode::ThreadIdx, GTy::Int, lam), lam));
        auto len = node(GNode::Len, GTy::Int, lam);
        len->slot = 1;
        auto cond = node(GNode::Bin, GTy::Bool, lam);
        cond->op = GB_Lt;
        cond->kids.push_back(var(iv, GTy::Int));
        cond->kids.push_back(std::move(len));
        auto then = node(GNode::Block, GTy::Void, lam);
        auto load = node(GNode::Load, elem, lam);
        load->slot = 0;
        load->kids.push_back(var(iv, GTy::Int));
        then->kids.push_back(setVar(vv, std::move(load), lam));
        auto store = node(GNode::Store, GTy::Void, lam);
        store->slot = 1;
        store->kids.push_back(var(iv, GTy::Int));
        store->kids.push_back(std::move(e));
        then->kids.push_back(std::move(store));
        auto ifn = node(GNode::If, GTy::Void, lam);
        ifn->kids.push_back(std::move(cond));
        ifn->kids.push_back(std::move(then));
        ifn->kids.push_back(node(GNode::Block, GTy::Void, lam));
        body->kids.push_back(std::move(ifn));
        k.body = std::move(body);
        return err.empty();
    }
};

// ------------------------------------------------------------------ the CPU fallback

// The kernel as register code (gpukern.hpp): each variable is a register,
// temporaries follow; one tight loop runs it for every thread, on every core,
// as machine code where the JIT runs (kcode.cpp).
struct CpuCompiler {
    const GpuKernel& k;
    CpuCode out;
    int temp = 0;
    std::vector<std::vector<size_t>> breaks, conts;
    explicit CpuCompiler(const GpuKernel& kk) : k(kk) {
        temp = (int)k.vars.size();
        out.wide = k.wide;
        out.groups = k.groups;
    }
    int tmp() {
        int r = temp++;
        out.regs = std::max(out.regs, temp);
        return r;
    }
    size_t emit(COp op, int d = 0, int a = 0, int b = 0, uint8_t m = 0, uint8_t m2 = 0) {
        out.code.push_back(CInst{op, m, m2, d, a, b});
        return out.code.size() - 1;
    }
    int constant(const GNode& n) {
        int r = tmp();
        if (out.wide && n.k != GNode::BoolK) {
            uint64_t bits;
            if (n.k == GNode::FloatK) std::memcpy(&bits, &n.fval, 8);
            else bits = (uint64_t)n.ival;
            out.consts.push_back(bits);
            emit(C_CONSTW, r, (int32_t)out.consts.size() - 1);
        } else if (n.k == GNode::FloatK) {
            float f = (float)n.fval;
            int32_t bits;
            std::memcpy(&bits, &f, 4);
            emit(C_CONST, r, bits);
        } else {
            emit(C_CONST, r, (int32_t)n.ival);
        }
        return r;
    }
    int expr(const GNode& n) {
        switch (n.k) {
            case GNode::IntK: case GNode::BoolK: case GNode::FloatK: return constant(n);
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
            case GNode::SLoad: {
                int i = expr(*n.kids[0]);
                int r = tmp();
                emit(C_SLOAD, r, n.slot, i);
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
                emit(C_MATH, r, x, y, (uint8_t)n.op, (uint8_t)((n.kids[0]->t == GTy::Int ? 1 : 0) | (n.t == GTy::Int ? 2 : 0)));
                return r;
            }
            case GNode::Bin: {
                int x = expr(*n.kids[0]);
                int y = expr(*n.kids[1]);
                int r = tmp();
                GTy t = n.kids[0]->t;
                bool f = t == GTy::Float;
                COp op = C_IADD;
                uint8_t m2 = 0;
                switch (n.op) {
                    case GB_Add: op = f ? C_FADD : C_IADD; break;
                    case GB_Sub: op = f ? C_FSUB : C_ISUB; break;
                    case GB_Mul: op = f ? C_FMUL : C_IMUL; break;
                    case GB_Div:
                        op = C_FDIV;
                        m2 = n.ival ? 1 : 0;  // (wide) ints: 0 stops
                        break;
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
                emit(op, r, x, y, 0, m2);
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
            case GNode::SStore: {
                int i = expr(*n.kids[0]);
                int v = expr(*n.kids[1]);
                emit(C_SSTORE, n.slot, i, v);
                break;
            }
            case GNode::Barrier: emit(C_BAR); break;
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
void mathOp(uint8_t op, bool ints, GWord x, GWord y, GWord& out) {
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
}
// the instructions machine code leaves to C++ (narrow)
void narrowOp(const CInst& in, GWord* r) {
    switch (in.op) {
        case C_IDIV: r[in.d].i = floorDiv(r[in.a].i, r[in.b].i); break;
        case C_IMOD: r[in.d].i = floorMod(r[in.a].i, r[in.b].i); break;
        case C_FIDIV: r[in.d].f = std::floor(r[in.a].f / r[in.b].f); break;
        case C_FMOD: {
            float x = r[in.a].f, y = r[in.b].f;
            r[in.d].f = x - y * std::floor(x / y);
            break;
        }
        case C_FPOW: r[in.d].f = std::pow(r[in.a].f, r[in.b].f); break;
        case C_MATH: {
            GWord o{};
            mathOp(in.m, (in.m2 & 1) != 0, r[in.a], r[in.b], o);
            r[in.d] = o;
            break;
        }
        default: break;
    }
}

struct CpuProgram {
    CpuCode code;
    KernelFn fn = nullptr;  // machine code (null: the register code runs)
};

struct CpuRun {
    const GpuKernel& k;
    const CpuProgram& p;
    const std::vector<ObjGpuBuffer*>& bufs;  // by parameter index (null for scalars)
    const std::vector<GWord>& init;          // the scalar parameters' values
    std::atomic<int64_t>* bad;
    void bounds(int32_t i, int b) {
        int64_t none = -1;
        bad->compare_exchange_strong(none, (int64_t)(uint32_t)i | ((int64_t)b << 32));
    }
    // runs a thread from pc until it ends (false) or reaches a barrier (true)
    bool step(int32_t tid, GWord* r, size_t& pc, GWord* const* shared) {
        const CInst* code = p.code.code.data();
        for (;;) {
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
                case C_SLOAD: {
                    int32_t i = r[in.b].i;
                    if (!shared || (uint32_t)i >= k.shared[(size_t)in.a].second) {
                        bounds(i, (int)k.params.size() + in.a);
                        r[in.d].u = 0;
                    } else {
                        r[in.d] = shared[in.a][i];
                    }
                    break;
                }
                case C_SSTORE: {
                    int32_t i = r[in.a].i;
                    if (!shared || (uint32_t)i >= k.shared[(size_t)in.d].second) bounds(i, (int)k.params.size() + in.d);
                    else shared[in.d][i] = r[in.b];
                    break;
                }
                case C_BAR: return true;
                case C_IADD: r[in.d].u = r[in.a].u + r[in.b].u; break;
                case C_ISUB: r[in.d].u = r[in.a].u - r[in.b].u; break;
                case C_IMUL: r[in.d].u = r[in.a].u * r[in.b].u; break;
                case C_FADD: r[in.d].f = r[in.a].f + r[in.b].f; break;
                case C_FSUB: r[in.d].f = r[in.a].f - r[in.b].f; break;
                case C_FMUL: r[in.d].f = r[in.a].f * r[in.b].f; break;
                case C_FDIV: r[in.d].f = r[in.a].f / r[in.b].f; break;
                case C_IDIV: case C_IMOD: case C_FIDIV: case C_FMOD: case C_FPOW: case C_MATH: narrowOp(in, r); break;
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
                case C_JMP: pc = (size_t)in.a; break;
                case C_JZ:
                    if (!r[in.a].i) pc = (size_t)in.b;
                    break;
                case C_RET: return false;
                default: break;
            }
        }
    }
    void run(int32_t tid, GWord* r) {
        for (size_t s = 0; s < init.size(); s++) r[s] = init[s];
        size_t pc = 0;
        step(tid, r, pc, nullptr);
    }
};

// what machine code calls for the rarer instructions (narrow)
void narrowHelper(void* regs, const CInst* in, KCtx* ctx) {
    auto* r = static_cast<GWord*>(regs);
    auto* run = static_cast<CpuRun*>(ctx->run);
    switch (in->op) {
        case C_LOAD:
            run->bounds(r[in->b].i, in->a);
            r[in->d].u = 0;
            break;
        case C_STORE: run->bounds(r[in->a].i, in->d); break;
        default: narrowOp(*in, r); break;
    }
}

// Ixo's own arithmetic (64-bit), for maps moved to all cores: exactly what the
// loop gives. Something the loop would stop at (dividing an int by 0) sets the
// trap, and the loop runs instead to stop there.
struct WideRun {
    explicit WideRun(const CpuProgram& pp) : p(pp) {}
    const CpuProgram& p;
    KCtx ctx{};
    std::vector<GWide> init;
    std::atomic<bool> trap{false};
};
void wideOp(const CInst& in, GWide* r, WideRun* run) {
    switch (in.op) {
        case C_IDIV: case C_IMOD: {
            int64_t a = r[in.a].i, b = r[in.b].i;
            if (b == 0) {
                run->trap = true;
                r[in.d].i = 0;
                break;
            }
            if (in.op == C_IDIV) {
                int64_t q = b == -1 ? (int64_t)(0 - (uint64_t)a) : a / b;
                if (b != -1 && (a % b != 0) && ((a < 0) != (b < 0))) q--;
                r[in.d].i = q;
            } else {
                int64_t m = b == -1 ? 0 : a % b;
                if (m != 0 && ((m < 0) != (b < 0))) m += b;
                r[in.d].i = m;
            }
            break;
        }
        case C_FDIV:
            if (in.m2 && r[in.b].f == 0) run->trap = true;
            r[in.d].f = r[in.a].f / r[in.b].f;
            break;
        case C_FIDIV:
            if (r[in.b].f == 0) run->trap = true;
            r[in.d].f = std::floor(r[in.a].f / r[in.b].f);
            break;
        case C_FMOD: {
            double y = r[in.b].f;
            if (y == 0) {
                run->trap = true;
                r[in.d].f = 0;
                break;
            }
            double m = std::fmod(r[in.a].f, y);
            if (m != 0 && ((m < 0) != (y < 0))) m += y;
            r[in.d].f = m;
            break;
        }
        case C_FPOW: r[in.d].f = std::pow(r[in.a].f, r[in.b].f); break;
        case C_MATH: {
            bool intIn = in.m2 & 1, intOut = in.m2 & 2;
            GWide x = r[in.a], y = r[in.b];
            GWide o{};
            switch (in.m) {
                case GM_Sqrt: o.f = std::sqrt(x.f); break;
                case GM_Abs:
                    if (intIn) o.i = x.i < 0 ? (int64_t)(0 - (uint64_t)x.i) : x.i;
                    else o.f = std::fabs(x.f);
                    break;
                case GM_Min:
                    if (intIn) o.i = y.i < x.i ? y.i : x.i;
                    else o.f = y.f < x.f ? y.f : x.f;
                    break;
                case GM_Max:
                    if (intIn) o.i = y.i > x.i ? y.i : x.i;
                    else o.f = y.f > x.f ? y.f : x.f;
                    break;
                case GM_Floor:
                    if (intOut) o.i = (int64_t)std::floor(x.f);
                    else o.f = std::floor(x.f);
                    break;
                case GM_Ceil:
                    if (intOut) o.i = (int64_t)std::ceil(x.f);
                    else o.f = std::ceil(x.f);
                    break;
                case GM_Sin: o.f = std::sin(x.f); break;
                case GM_Cos: o.f = std::cos(x.f); break;
                case GM_Tan: o.f = std::tan(x.f); break;
                case GM_Exp: o.f = std::exp(x.f); break;
                case GM_Log: o.f = std::log(x.f); break;
                case GM_Pow: o.f = std::pow(x.f, y.f); break;
                case GM_ToFloat: o.f = (double)x.i; break;
                case GM_ToInt: o.i = (int64_t)x.f; break;
            }
            r[in.d] = o;
            break;
        }
        case C_LOAD: case C_STORE: run->trap = true; break;  // (a map reads and writes its own place)
        default: break;
    }
}
void wideHelper(void* regs, const CInst* in, KCtx* ctx) { wideOp(*in, static_cast<GWide*>(regs), static_cast<WideRun*>(ctx->run)); }
// the register code, wide (without machine code)
void wideRun(WideRun& w, int64_t tid, GWide* r) {
    const CInst* code = w.p.code.code.data();
    const uint64_t* consts = w.p.code.consts.data();
    for (size_t pc = 0;;) {
        const CInst& in = code[pc++];
        switch (in.op) {
            case C_CONST: r[in.d].i = in.a; break;
            case C_CONSTW: r[in.d].u = consts[in.a]; break;
            case C_MOV: r[in.d] = r[in.a]; break;
            case C_TID: r[in.d].i = tid; break;
            case C_LEN: r[in.d].i = (int64_t)w.ctx.len[in.a]; break;
            case C_LOAD: {
                int64_t i = r[in.b].i;
                if ((uint64_t)i >= w.ctx.len[in.a]) wideOp(in, r, &w);
                else r[in.d].u = static_cast<uint64_t*>(w.ctx.data[in.a])[i];
                break;
            }
            case C_STORE: {
                int64_t i = r[in.a].i;
                if ((uint64_t)i >= w.ctx.len[in.d]) wideOp(in, r, &w);
                else static_cast<uint64_t*>(w.ctx.data[in.d])[i] = r[in.b].u;
                break;
            }
            case C_IADD: r[in.d].u = r[in.a].u + r[in.b].u; break;
            case C_ISUB: r[in.d].u = r[in.a].u - r[in.b].u; break;
            case C_IMUL: r[in.d].u = r[in.a].u * r[in.b].u; break;
            case C_FADD: r[in.d].f = r[in.a].f + r[in.b].f; break;
            case C_FSUB: r[in.d].f = r[in.a].f - r[in.b].f; break;
            case C_FMUL: r[in.d].f = r[in.a].f * r[in.b].f; break;
            case C_FDIV: case C_IDIV: case C_IMOD: case C_FIDIV: case C_FMOD: case C_FPOW: case C_MATH: wideOp(in, r, &w); break;
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
            case C_SHL: r[in.d].u = r[in.a].u << (r[in.b].u & 63); break;
            case C_SHR: r[in.d].i = r[in.a].i >> (r[in.b].u & 63); break;
            case C_NOT: r[in.d].i = !r[in.a].i; break;
            case C_INEG: r[in.d].u = 0 - r[in.a].u; break;
            case C_FNEG: r[in.d].f = -r[in.a].f; break;
            case C_JMP: pc = (size_t)in.a; break;
            case C_JZ:
                if (!r[in.a].i) pc = (size_t)in.b;
                break;
            case C_RET: return;
            default: return;
        }
    }
}

bool kernelMachineCodeOn() {
    static const bool on = [] {
        const char* e = std::getenv("IXO_GPU_JIT");
        return !(e && e[0] == '0');
    }();
    return on;
}

const CpuProgram& cpuCodeFor(GpuKernel& k) {
    static std::mutex lock;
    static std::map<const GpuKernel*, std::unique_ptr<CpuProgram>> cache;
    std::lock_guard<std::mutex> g(lock);
    auto& c = cache[&k];
    if (!c) {
        CpuCompiler cc(k);
        cc.out.regs = std::max(cc.out.regs, (int)k.vars.size());
        cc.stmt(*k.body);
        cc.emit(C_RET);
        c = std::make_unique<CpuProgram>();
        c->code = std::move(cc.out);
        if (kernelMachineCodeOn()) c->fn = kernelMachineCode(c->code, k.wide ? wideHelper : narrowHelper);
    }
    return *c;
}

template <class F>
void onCores(uint32_t n, uint32_t minSplit, F f) {
    unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    if (n < minSplit || cores == 1) {
        f(0u, n);
        return;
    }
    std::vector<std::thread> pool;
    uint32_t per = (n + cores - 1) / cores;
    for (unsigned c = 1; c < cores; c++) {
        uint32_t lo = c * per, hi = std::min(n, lo + per);
        if (lo >= hi) break;
        pool.emplace_back(f, lo, hi);
    }
    f(0u, std::min(n, per));
    for (auto& th : pool) th.join();
}

// Kernels with barriers or shared arrays: a group of 64 at a time. Its threads
// take turns, each running until it reaches the next barrier (or ends).
void runGroups(CpuRun& r, uint32_t n) {
    uint32_t groups = (n + 63) / 64;
    onCores(groups, 2, [&r](uint32_t g0, uint32_t g1) {
        int R = std::max(r.p.code.regs, 1);
        std::vector<GWord> regs((size_t)R * 64);
        std::vector<std::vector<GWord>> shared(r.k.shared.size());
        std::vector<GWord*> sp(shared.size());
        size_t pcs[64];
        bool live[64];
        for (uint32_t g = g0; g < g1; g++) {
            for (size_t s = 0; s < shared.size(); s++) {
                shared[s].assign(r.k.shared[s].second, GWord{});
                sp[s] = shared[s].data();
            }
            for (int t = 0; t < 64; t++) {
                GWord* tr = &regs[(size_t)t * (size_t)R];
                for (size_t s = 0; s < r.init.size(); s++) tr[s] = r.init[s];
                pcs[t] = 0;
                live[t] = true;
            }
            for (bool any = true; any;) {
                any = false;
                for (int t = 0; t < 64; t++) {
                    if (!live[t]) continue;
                    if (r.step((int32_t)(g * 64 + (uint32_t)t), &regs[(size_t)t * (size_t)R], pcs[t], sp.data())) any = true;
                    else live[t] = false;
                }
            }
        }
    });
}

void runOnCpu(CpuRun& r, uint32_t n) {
    if (r.p.code.groups) {
        runGroups(r, n);
        return;
    }
    KCtx base{};
    bool mc = r.p.fn && r.bufs.size() <= (size_t)kKernelBuffers;
    if (mc)
        for (size_t i = 0; i < r.bufs.size(); i++)
            if (r.bufs[i]) {
                base.data[i] = r.bufs[i]->data;
                base.len[i] = r.bufs[i]->n;
            }
    base.run = &r;
    onCores(n, 4096, [&r, &base, mc](uint32_t lo, uint32_t hi) {
        std::vector<GWord> regs((size_t)std::max(r.p.code.regs, 1));
        if (mc) {
            for (uint32_t t = lo; t < hi; t++) {
                for (size_t s = 0; s < r.init.size(); s++) regs[s] = r.init[s];
                r.p.fn(regs.data(), &base, (int64_t)t);
            }
        } else {
            for (uint32_t t = lo; t < hi; t++) r.run((int32_t)t, regs.data());
        }
    });
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
int64_t pendingW = 0, pendingH = 0;  // gpu.launch((w, h), f, ...): its width and height (gpu.x, gpu.y)

#ifdef __EMSCRIPTEN__
// In the browser playground, kernels run through WebGPU. A worker (gpu-worker.js)
// does the asynchronous WebGPU calls in Ixo's shared memory. Ixo's own thread
// blocks while it waits, so the runtime's main thread (which stays free) starts
// that worker and hands it the work; Ixo's thread waits on a flag: 1 done, 2 no WebGPU.
int ixo_webgpu_run(const char* wgsl, int wgslLen, const uint32_t* desc, int descLen, int32_t* flag) {
    __atomic_store_n(flag, 0, __ATOMIC_SEQ_CST);
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            var flag = $4;
            var fail = function() {
                var f = new Int32Array(HEAPU8.buffer, flag, 1);
                Atomics.store(f, 0, 2);
                Atomics.notify(f, 0);
            };
            try {
                if (!globalThis.__ixoGpu) globalThis.__ixoGpu = new Worker(new URL('gpu-worker.js', location.href));
                globalThis.__ixoGpu.onerror = fail;
                globalThis.__ixoGpu.postMessage({mem: HEAPU8.buffer, wgsl: UTF8ToString($0, $1),
                                                 desc: Array.from(HEAPU32.subarray($2 >> 2, ($2 >> 2) + $3)), flag: flag});
            } catch (e) {
                fail();
            }
        },
        wgsl, wgslLen, desc, descLen, flag);
    double start = emscripten_get_now();
    while (__atomic_load_n(flag, __ATOMIC_SEQ_CST) == 0) {
        if (emscripten_get_now() - start > 60000) return 2;  // nothing came back: the CPU from now on
        emscripten_futex_wait(flag, 0, 1000);
    }
    return __atomic_load_n(flag, __ATOMIC_SEQ_CST);
}
int webgpuState = 0;  // 0 not tried, 1 works, 2 not available
#endif

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
#ifdef __EMSCRIPTEN__
    if (webgpuState == 1) return vm.vstr("webgpu");
#endif
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
static Value gpu_launch(VM& vm, Value* a, int n);

// xs.map(fn(v) = ...) on a big list of numbers, with a lambda that only computes:
// __auto_map(kernel for floats, kernel for ints, items, captured values...) runs it
// on every core, with Ixo's own (64-bit) arithmetic, so the result is exactly the
// loop's. undef: not worth it or not possible here (the loop runs instead).
NATIVE(gpu_auto_map) {
    static const size_t minItems = [] {
        const char* e = std::getenv("IXO_AUTO_PAR");
        if (e && e[0] == '0' && !e[1]) return SIZE_MAX;  // IXO_AUTO_PAR=0: never
        return e ? (size_t)std::max<long long>(1, std::atoll(e)) : (size_t)200000;
    }();
    static const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    Value items = a[2];
    if (vm.isWorker || vm.checkOverflow || cores < 2 || !isObjType(items, OT::List)) return Value::undef();
    auto* l = as<ObjList>(items);
    size_t len = l->len();
    if (len < minItems || len > INT32_MAX) return Value::undef();
    int kid = l->lk() == LK_INTS ? (int)a[1].i : l->lk() == LK_FLOATS ? (int)a[0].i : -1;
    GpuKernel* k = kid >= 0 ? gpuKernel(kid) : nullptr;
    if (!k || !k->wide || (int)k->params.size() != 2 + (n - 3)) return Value::undef();
    const CpuProgram& prog = cpuCodeFor(*k);
    WideRun w(prog);
    for (int i = 3; i < n; i++) {  // captured values: exactly the types compiled for
        GTy t = k->params[(size_t)(2 + i - 3)].t;
        Value v = a[i];
        GWide x{};
        if (t == GTy::Int && v.t == VT::Int) x.i = v.i;
        else if (t == GTy::Float && v.t == VT::Float) x.f = v.f;
        else if (t == GTy::Bool && v.t == VT::Bool) x.i = v.b ? 1 : 0;
        else return Value::undef();
        w.init.push_back(x);
    }
    GTy outT = k->params[1].elem;
    auto* out = vm.alloc<ObjList>();
    Value keep = Value::object(out);
    vm.extraRoots.push_back(keep);
    out->become(outT == GTy::Float ? LK_FLOATS : LK_INTS);
    if (outT == GTy::Float) out->floats.resize(len);
    else out->ints.resize(len);
    vm.grow(out, len * 8);
    w.ctx.data[0] = l->lk() == LK_INTS ? (void*)l->ints.data() : (void*)l->floats.data();
    w.ctx.len[0] = len;
    w.ctx.data[1] = outT == GTy::Float ? (void*)out->floats.data() : (void*)out->ints.data();
    w.ctx.len[1] = len;
    w.ctx.run = &w;
    onCores((uint32_t)len, 1, [&w, &prog](uint32_t lo, uint32_t hi) {
        std::vector<GWide> regs((size_t)std::max(prog.code.regs, 1));
        for (uint32_t t = lo; t < hi && !w.trap.load(std::memory_order_relaxed); t++) {
            for (size_t s = 0; s < w.init.size(); s++) regs[s] = w.init[s];
            if (prog.fn) prog.fn(regs.data(), &w.ctx, (int64_t)t);
            else wideRun(w, (int64_t)t, regs.data());
        }
    });
    vm.extraRoots.pop_back();
    if (w.trap) return Value::undef();  // the loop runs and stops where it should
    if (k->outBool) {
        std::vector<uint8_t> bools(len);
        for (size_t i = 0; i < len; i++) bools[i] = out->ints[i] != 0;
        out->become(LK_BOOLS);
        out->bools = std::move(bools);
    }
    if (isArray(items)) out->appendable = 1;
    return keep;
}

// gpu xs.map(fn(v) = ...): __gpu_map(kernel for float items, kernel for int items (or -1),
// items, captured values...) gives the results as a list (an array for an array)
NATIVE(gpu_map) {
    Value items = a[2];
    if (isObjType(items, OT::Range)) {
        auto* r = as<ObjRange>(items);
        std::vector<Value> xs;
        for (int64_t i = r->start; r->step > 0 ? (r->inclusive ? i <= r->end : i < r->end) : (r->inclusive ? i >= r->end : i > r->end);
             i += r->step)
            xs.push_back(Value::integer(i));
        items = vm.newList(std::move(xs));
    }
    vm.extraRoots.push_back(items);
    bool ints = isObjType(items, OT::List) && as<ObjList>(items)->lk() == LK_INTS;
    if (!ints && isObjType(items, OT::List) && as<ObjList>(items)->lk() == LK_VALUES) {
        ints = true;
        for (size_t i = 0; i < as<ObjList>(items)->len(); i++)
            if (!as<ObjList>(items)->get(i).isInt()) ints = false;
    }
    int kid = (int)(ints && a[1].i >= 0 ? a[1].i : a[0].i);
    GpuKernel* k = gpuKernel(kid);
    if (!k) vm.panic("internal: unknown gpu map kernel");
    Value src = makeBuffer(vm, &items, k->params[0].elem == GTy::Float, "gpu map");
    vm.extraRoots.push_back(src);
    Value count = Value::integer(static_cast<ObjGpuBuffer*>(src.o)->n);
    Value dst = makeBuffer(vm, &count, k->params[1].elem == GTy::Float, "gpu map");
    vm.extraRoots.push_back(dst);
    std::vector<Value> args{Value::integer(kid), src, dst};
    for (int i = 3; i < n; i++) args.push_back(a[i]);
    gpu_launch(vm, args.data(), (int)args.size());
    Value out = buf_list(vm, &dst, 1);
    if (isArray(a[2])) as<ObjList>(out)->appendable = 1;
    vm.extraRoots.resize(vm.extraRoots.size() - 3);
    return out;
}
NATIVE(buf_upload) {
    fill(vm, bufferArg(vm, a[0], "upload"), a[1], "upload");
    return a[0];
}
NATIVE(buf_address) {  // for ffi: where the items are (raw memory: unsafe in strict code)
    auto* b = bufferArg(vm, a[0], "address");
    return Value::integer((int64_t)(uintptr_t)b->data);
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
    // the gpu fn's arguments as the kernel's parameters: a vec or a mat is several floats
    std::vector<Value> expanded;
    size_t want = k->args.empty() ? k->params.size() - (k->dims ? 2 : 0) : k->args.size();
    if ((size_t)(n - 1) != want) vm.panic(k->name + "() takes " + std::to_string(want) + " arguments");
    if (k->args.empty()) {
        expanded.assign(a + 1, a + n);
    } else {
        for (size_t i = 0; i < k->args.size(); i++) {
            const auto& ga = k->args[i];
            Value v = a[1 + i];
            if (ga.kind == 1) {
                if (!isObjType(v, OT::Vec) || as<ObjVec>(v)->v.size() != (size_t)ga.width)
                    vm.panic("argument '" + ga.name + "' of gpu fn " + k->name + " should be a vec of " + std::to_string(ga.width));
                for (double c : as<ObjVec>(v)->v) expanded.push_back(Value::number(c));
            } else if (ga.kind == 2) {
                if (!isObjType(v, OT::Mat) || as<ObjMat>(v)->rows != 4 || as<ObjMat>(v)->cols != 4)
                    vm.panic("argument '" + ga.name + "' of gpu fn " + k->name + " should be a 4x4 mat");
                for (double c : as<ObjMat>(v)->d) expanded.push_back(Value::number(c));
            } else {
                expanded.push_back(v);
            }
        }
    }
    int64_t dimW = pendingW, dimH = pendingH;
    pendingW = pendingH = 0;
    if (k->dims) {  // filled in below, once the thread count is known
        expanded.push_back(Value::integer(0));
        expanded.push_back(Value::integer(0));
    }
    Value* args = expanded.data();
    int argc = (int)expanded.size();
    std::vector<ObjGpuBuffer*> bufs(k->params.size(), nullptr);
    std::vector<GWord> init;
    std::vector<uint32_t> words;
    uint32_t threads = pendingThreads;
    bool sized = pendingThreads > 0;
    pendingThreads = 0;
    for (int i = 0; i < argc; i++) {
        const GParam& p = k->params[(size_t)i];
        if (k->dims && i == argc - 2) {  // the launch's width and height
            if (!sized) break;
            if (dimW <= 0) {
                dimW = threads;
                dimH = 1;
            }
            args[i] = Value::integer(dimW);
            args[i + 1] = Value::integer(dimH);
        }
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
#ifdef __EMSCRIPTEN__
    if (!done && threads > 0 && webgpuState != 2 && !std::getenv("IXO_GPU")) {
        // WebGPU: the bindings are the buffers, the status words, then a uniform with the scalars, n and the stride
        if (k->wgsl.empty()) k->wgsl = wgslFor(*k);
        static int32_t flag = 0;
        static int32_t status[4];
        std::memset(status, 0, sizeof status);
        uint32_t groupsN = (threads + 63) / 64, gx = std::min<uint32_t>(groupsN, 65535), gy = (groupsN + gx - 1) / gx;
        std::vector<uint32_t> uniform = words;
        uniform.push_back(threads);
        uniform.push_back(gx * 64);
        std::vector<uint32_t> desc;
        uint32_t nb = 0;
        for (auto* b : bufs) nb += b != nullptr;
        desc.push_back(nb);
        for (auto* b : bufs)
            if (b) {
                desc.push_back((uint32_t)(uintptr_t)b->data);
                desc.push_back(b->n * 4);
            }
        desc.push_back((uint32_t)(uintptr_t)uniform.data());
        desc.push_back((uint32_t)uniform.size() * 4);
        desc.push_back(gx);
        desc.push_back(gy);
        desc.push_back((uint32_t)(uintptr_t)status);
        int r = ixo_webgpu_run(k->wgsl.data(), (int)k->wgsl.size(), desc.data(), (int)desc.size(), &flag);
        if (r == 1) {
            webgpuState = 1;
            done = true;
            if (status[0]) bad = (int64_t)(uint32_t)status[1] | ((int64_t)status[2] << 32);
        } else {
            webgpuState = 2;
        }
    }
#endif
    if (!done && threads > 0) {
        std::atomic<int64_t> flag{-1};
        CpuRun r{*k, cpuCodeFor(*k), bufs, init, &flag};
        runOnCpu(r, threads);
        bad = flag.load();
    }
    if (bad >= 0) {
        int b = (int)(bad >> 32);
        int32_t idx = (int32_t)(uint32_t)(bad & 0xFFFFFFFF);
        if (b >= (int)k->params.size())
            vm.panic("gpu fn " + k->name + ": index " + std::to_string(idx) + " is out of range for a shared array of length " +
                     std::to_string(b - (int)k->params.size() < (int)k->shared.size() ? k->shared[(size_t)(b - (int)k->params.size())].second : 0));
        std::string name = b >= 0 && b < (int)k->params.size() ? k->params[(size_t)b].name : "a buffer";
        uint32_t len = b >= 0 && b < (int)bufs.size() && bufs[(size_t)b] ? bufs[(size_t)b]->n : 0;
        vm.panic("gpu fn " + k->name + ": index " + std::to_string(idx) + " is out of range for buffer '" + name +
                 "' of length " + std::to_string(len));
    }
    return Value::object(vm.finishedTask());  // already finished: launches run in order
}
// gpu.launch(n, kernel, args...): n threads
NATIVE(gpu_launch_n) {
    if (isObjType(a[0], OT::Tuple)) {  // gpu.launch((w, h), f, ...) or (w, h, d): gpu.x, gpu.y, gpu.z
        auto& t = as<ObjTuple>(a[0])->items;
        int64_t total = 1;
        if (t.size() < 2 || t.size() > 3) vm.panic("gpu.launch takes a count, or (width, height) or (width, height, depth)");
        for (auto& d : t) {
            if (!d.isInt() || d.i < 1) vm.panic("gpu.launch's sizes are ints from 1");
            total *= d.i;
        }
        if (total > INT32_MAX) vm.panic("gpu.launch: too many threads");
        pendingW = t[0].i;
        pendingH = t[1].i;
        if (!isObjType(a[1], OT::Closure) || as<ObjClosure>(a[1])->fn->gpuKernel < 0) vm.panic("gpu.launch expects a gpu fn");
        pendingThreads = (uint32_t)total;
        std::vector<Value> args(a + 2, a + n);
        Value f = a[1];
        return vm.callNested(f, args.data(), (int)args.size());
    }
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

int gpuCompileMap(FnDecl* lambda, const std::vector<std::pair<Decl*, GTy>>& captures, GTy elem, std::string& err, int& line,
                  int& col, bool wide) {
    auto k = std::make_unique<GpuKernel>();
    KernelCompiler kc(*k);
    kc.wide = wide;
    if (!kc.compileMap(lambda, captures, elem)) {
        err = kc.err.empty() ? "this lambda can't run on the GPU" : kc.err;
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
    vm.defineNative("__gpu_map", gpu_map, 3, -1, "fn(int, int, any, any...) -> list", "Runs gpu xs.map(...).");
    auto* g = vm.defineNamespace("gpu", "GPU compute: buffers and kernels written as gpu fn.");
    vm.nativeModules.push_back("gpu");
    vm.nsFunc(g, "floats", gpu_floats, 1, 1, "fn(any) -> gpu buffer", "A buffer of 32-bit floats: n zeros, or a list's numbers.");
    vm.nsFunc(g, "ints", gpu_ints, 1, 1, "fn(any) -> gpu buffer", "A buffer of 32-bit ints: n zeros, or a list's ints.");
    vm.nsFunc(g, "buffer", gpu_buffer, 1, 1, "fn(list) -> gpu buffer", "A buffer of a list's numbers: floats if any is a float, else ints.");
    vm.nsFunc(g, "launch", gpu_launch_n, 2, -1, "fn(any, fn, any...) -> task",
              "Runs a gpu fn over n threads, or (width, height[, depth]) of them (gpu.x, gpu.y, gpu.z).");
    vm.nsFunc(g, "devices", gpu_devices, 0, 0, "fn() -> list[str]", "The GPUs found (empty: kernels run on the CPU).");
    vm.nsFunc(g, "device", gpu_device, 0, 0, "fn() -> str", "The GPU kernels run on, or \"cpu\".");
    vm.nsFunc(g, "index", gpu_index, 0, 0, "fn() -> int", "Inside a gpu fn: this thread's position (0 to n - 1).");
    vm.nsFunc(g, "wgsl", gpu_wgsl, 1, 1, "fn(fn) -> str", "A gpu fn as WGSL, for WebGPU.");
    vm.nsFunc(g, "spirv", gpu_spirv, 1, 1, "fn(fn) -> list[int]", "A gpu fn as SPIR-V words, for Vulkan.");
    vm.defineMethod(Tag::GpuBuf, "len", buf_len, 0, 0, "fn() -> int", "Number of items.");
    vm.defineMethod(Tag::GpuBuf, "list", buf_list, 0, 0, "fn() -> list", "The items as a list.");
    vm.defineMethod(Tag::GpuBuf, "upload", buf_upload, 1, 1, "fn(list) -> gpu buffer", "Replaces the items with a list's.");
    vm.defineMethod(Tag::GpuBuf, "free", buf_free, 0, 0, "fn()", "Frees the buffer now (the collector would later).");
    vm.defineMethod(Tag::GpuBuf, "address", buf_address, 0, 0, "fn() -> int",
                    "Where the items are in memory, for ffi calls (raw memory: in an unsafe block in strict code).");
    vm.defineNative("__auto_map", gpu_auto_map, 3, -1, "fn(int, int, any, any...) -> any", "");
    registerCuda(vm, g);
}

}  // namespace ixo
