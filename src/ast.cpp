#include <algorithm>
#include <unordered_set>

#include "parser.hpp"

namespace ixo {

std::string TypeExpr::str() const {
    std::string s;
    switch (k) {
        case Named:
            s = name;
            if (!args.empty()) {
                s += "[";
                for (size_t i = 0; i < args.size(); i++) s += (i ? ", " : "") + args[i]->str();
                s += "]";
            }
            return s;
        case Tuple:
            s = "(";
            for (size_t i = 0; i < args.size(); i++) s += (i ? ", " : "") + args[i]->str();
            if (args.size() == 1) s += ",";
            return s + ")";
        case Fn:
            s = "fn(";
            for (size_t i = 0; i < args.size(); i++) s += (i ? ", " : "") + args[i]->str();
            s += ")";
            if (ret) s += " -> " + ret->str();
            return s;
        case Optional: return args[0]->str() + "?";
        case Result: return args[0]->str() + " ! " + (ret ? ret->str() : "Error");
        case Ref: return std::string(mut ? "&mut " : "&") + args[0]->str();
    }
    return "?";
}

const char* binOpText(BinOp op) {
    switch (op) {
        case BinOp::None: return "=";
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::Mul: return "*";
        case BinOp::Div: return "/";
        case BinOp::Mod: return "%";
        case BinOp::Pow: return "**";
        case BinOp::Root: return "root";
        case BinOp::IntDiv: return "//";
        case BinOp::Eq: return "==";
        case BinOp::Ne: return "!=";
        case BinOp::Lt: return "<";
        case BinOp::Le: return "<=";
        case BinOp::Gt: return ">";
        case BinOp::Ge: return ">=";
        case BinOp::Is: return "is";
        case BinOp::IsNot: return "is not";
        case BinOp::In: return "in";
        case BinOp::NotIn: return "not in";
        case BinOp::And: return "and";
        case BinOp::Or: return "or";
        case BinOp::BitAnd: return "&";
        case BinOp::BitOr: return "|";
        case BinOp::BitXor: return "^";
        case BinOp::Shl: return "<<";
        case BinOp::Shr: return ">>";
        case BinOp::Send: return ":>";
    }
    return "?";
}

bool isBuiltinTypeName(const std::string& n) {
    static const std::unordered_set<std::string> names = {
        "int", "float", "str", "bool", "list", "map", "set", "tuple", "fn", "range", "flow",
        "task", "vec", "mat", "quat", "time", "rng", "any", "num", "void", "iter", "module",
    };
    return names.count(n) > 0;
}

static void forEachPatternChild(Pattern* p, const std::function<void(Node*)>& f) {
    if (!p) return;
    if (p->value) f(p->value.get());
    if (p->value2) f(p->value2.get());
    for (auto& k : p->keys) f(k.get());
    for (auto& s : p->subs) forEachPatternChild(s.get(), f);
}

void forEachChild(Node* n, const std::function<void(Node*)>& f) {
    if (!n) return;
    auto E = [&](const ExprP& e) {
        if (e) f(e.get());
    };
    auto B = [&](const std::unique_ptr<Block>& b) {
        if (b) f(b.get());
    };
    switch (n->kind) {
        case NK::IntLit: case NK::FloatLit: case NK::StrLit: case NK::BoolLit: case NK::NilLit:
        case NK::Break: case NK::Continue: case NK::Todo: case NK::Use:
            break;
        case NK::FStr:
            for (auto& p : static_cast<FStr*>(n)->parts) E(p.expr);
            break;
        case NK::Unary: E(static_cast<Unary*>(n)->operand); break;
        case NK::Binary: {
            auto* b = static_cast<Binary*>(n);
            E(b->left);
            E(b->right);
            break;
        }
        case NK::Range: {
            auto* r = static_cast<Range*>(n);
            E(r->start);
            E(r->end);
            E(r->step);
            break;
        }
        case NK::Call: {
            auto* c = static_cast<Call*>(n);
            E(c->callee);
            for (auto& a : c->args) E(a);
            break;
        }
        case NK::NamedArg: E(static_cast<NamedArg*>(n)->value); break;
        case NK::Select:
            for (auto& a : static_cast<Select*>(n)->arms) {
                E(a.expr);
                B(a.body);
            }
            break;
        case NK::Member: {
            auto* m = static_cast<Member*>(n);
            E(m->object);
            for (auto& a : m->args) E(a);
            break;
        }
        case NK::Index: {
            auto* x = static_cast<Index*>(n);
            E(x->object);
            E(x->index);
            break;
        }
        case NK::ListLit: for (auto& e : static_cast<ListLit*>(n)->elems) E(e); break;
        case NK::TupleLit: for (auto& e : static_cast<TupleLit*>(n)->elems) E(e); break;
        case NK::SetLit: for (auto& e : static_cast<SetLit*>(n)->elems) E(e); break;
        case NK::MapLit: {
            auto* m = static_cast<MapLit*>(n);
            for (size_t i = 0; i < m->keys.size(); i++) {
                E(m->keys[i]);
                E(m->values[i]);
            }
            break;
        }
        case NK::Lambda: f(static_cast<Lambda*>(n)->fn.get()); break;
        case NK::Ident: E(static_cast<Ident*>(n)->selfMember); break;
        case NK::If: {
            auto* x = static_cast<If*>(n);
            E(x->cond);
            B(x->then);
            B(x->otherwise);
            break;
        }
        case NK::Match: {
            auto* m = static_cast<Match*>(n);
            E(m->subject);
            for (auto& arm : m->arms) {
                forEachPatternChild(arm.pat.get(), f);
                E(arm.guard);
                B(arm.body);
            }
            break;
        }
        case NK::Catch: {
            auto* c = static_cast<Catch*>(n);
            E(c->expr);
            B(c->handler);
            break;
        }
        case NK::Try: E(static_cast<Try*>(n)->expr); break;
        case NK::Fork: E(static_cast<Fork*>(n)->call); break;
        case NK::ExprStmt: E(static_cast<ExprStmt*>(n)->expr); break;
        case NK::Assign: {
            auto* a = static_cast<Assign*>(n);
            E(a->target);
            E(a->value);
            break;
        }
        case NK::Destructure: {
            auto* d = static_cast<Destructure*>(n);
            for (auto& t : d->targets) E(t);
            E(d->value);
            break;
        }
        case NK::VarDecl: E(static_cast<VarDecl*>(n)->init); break;
        case NK::IncDec: E(static_cast<IncDec*>(n)->target); break;
        case NK::Block:
            for (auto& s : static_cast<Block*>(n)->stmts) f(s.get());
            break;
        case NK::While: {
            auto* w = static_cast<While*>(n);
            E(w->cond);
            B(w->body);
            break;
        }
        case NK::For: {
            auto* x = static_cast<For*>(n);
            E(x->iter);
            E(x->step);
            B(x->body);
            break;
        }
        case NK::ForRecv: {
            auto* x = static_cast<ForRecv*>(n);
            for (auto& c : x->chans) E(c);
            B(x->body);
            break;
        }
        case NK::Return: E(static_cast<Return*>(n)->value); break;
        case NK::Retry: {
            auto* r = static_cast<Retry*>(n);
            E(r->count);
            E(r->delay);
            break;
        }
        case NK::FnDecl: {
            auto* fn = static_cast<FnDecl*>(n);
            for (auto& p : fn->params) E(p.def);
            B(fn->body);
            break;
        }
        case NK::ClassDecl: {
            auto* c = static_cast<ClassDecl*>(n);
            for (auto& p : c->ctorParams) E(p.def);
            for (auto& fd : c->fields) E(fd.def);
            for (auto& m : c->methods) f(m.get());
            break;
        }
        case NK::With: B(static_cast<With*>(n)->body); break;
        case NK::Region:
            E(static_cast<Region*>(n)->init);
            B(static_cast<Region*>(n)->body);
            break;
        case NK::Defer: B(static_cast<Defer*>(n)->body); break;
        case NK::InterfaceDecl:
            for (auto& m : static_cast<InterfaceDecl*>(n)->methods) f(m.get());
            break;
        case NK::EnumDecl: {
            auto* e = static_cast<EnumDecl*>(n);
            for (auto& v : e->variants) f(v.cls.get());
            for (auto& m : e->methods) f(m.get());
            break;
        }
        case NK::Recv: {
            auto* r = static_cast<Recv*>(n);
            E(r->target);
            E(r->chan);
            break;
        }
    }
}

static void collectRec(Node* n, std::vector<std::string>& out) {
    if (!n) return;
    if (n->kind == NK::Lambda || n->kind == NK::FnDecl) return;
    if (n->kind == NK::Ident) {
        auto* id = static_cast<Ident*>(n);
        if (id->isPlaceholder() && std::find(out.begin(), out.end(), id->name) == out.end())
            out.push_back(id->name);
        return;
    }
    forEachChild(n, [&](Node* c) { collectRec(c, out); });
}

void collectPlaceholders(Node* n, std::vector<std::string>& out) {
    if (!n) return;
    if (n->kind == NK::FnDecl) {  // the body of an arrow lambda being built
        forEachChild(n, [&](Node* c) { collectRec(c, out); });
        return;
    }
    collectRec(n, out);
}

Select::~Select() = default;

}  // namespace ixo
