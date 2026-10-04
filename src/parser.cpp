#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace ixo {

// @temp, @frame, @req and @thread were the same as @arena
static bool isOldRegionName(const std::string& s) { return s == "temp" || s == "frame" || s == "req" || s == "thread"; }

static bool isRegionKind(const std::string& s) {
    static const std::unordered_set<std::string> kinds = {"arena", "temp", "frame", "dual",
                                                          "req",   "cache", "thread"};
    return kinds.count(s) > 0;
}

Parser::Parser(std::vector<Token> tokens, std::string file, Diagnostics& diags, ParseOptions opts)
    : toks(std::move(tokens)), file(std::move(file)), diags(diags), opts(opts) {
    // Comments are only needed by the formatter.
    toks.erase(std::remove_if(toks.begin(), toks.end(), [](const Token& t) { return t.kind == T::Comment; }),
               toks.end());
    if (toks.empty() || toks.back().kind != T::Eof) {
        Token eof;
        eof.kind = T::Eof;
        toks.push_back(eof);
    }
}

static std::string describe(const Token& t) {
    switch (t.kind) {
        case T::Ident: return "'" + t.text + "'";
        case T::Int:
        case T::Float: return "number " + t.text;
        case T::Str: return "a string";
        default: return tokenName(t.kind);
    }
}

void Parser::fail(const Token& at, const std::string& msg) {
    if (quiet == 0) {
        int len = at.end > at.start ? (int)(at.end - at.start) : 1;
        diags.error(file, {at.line, at.col}, msg, len);
    }
    throw ParseError{};
}

void Parser::failExpected(const char* what) {
    fail(peek(), std::string("expected ") + what + ", found " + describe(peek()));
}

const Token& Parser::expect(T k, const char* what) {
    if (!check(k)) failExpected(what);
    return advance();
}

void Parser::deprecate(const Token& at, const std::string& msg, std::vector<TextEdit> fix) {
    if (quiet > 0) return;
    int len = at.end > at.start ? (int)(at.end - at.start) : 1;
    diags.deprecated(file, {at.line, at.col}, msg, len, std::move(fix));
}

std::string Parser::spaced(const Token& t, const std::string& word) const {
    size_t k = (size_t)(&t - toks.data());
    const Token* next = k + 1 < toks.size() ? &toks[k + 1] : nullptr;
    return (t.spaceBefore || t.newlineBefore ? "" : " ") + word + (next && !next->spaceBefore ? " " : "");
}

static TextEdit replaceTok(const Token& t, std::string text) { return {t.start, t.end, std::move(text)}; }

void Parser::synchronize() {
    int depth = 0;
    while (!check(T::Eof)) {
        if (check(T::Indent)) {
            depth++;
        } else if (check(T::Dedent)) {
            if (depth == 0) return;
            depth--;
        } else if (check(T::Newline) && depth == 0) {
            advance();
            return;
        }
        advance();
    }
}

void Parser::endStatement() {
    T p = prev().kind;
    if (p == T::Dedent || p == T::Newline) return;
    if (match(T::Newline)) return;
    if (check(T::Dedent) || check(T::Eof)) return;
    failExpected("end of line");
}

std::unique_ptr<Block> Parser::parseProgram() {
    auto block = std::make_unique<Block>(SrcPos{1, 1});
    while (!check(T::Eof)) {
        if (match(T::Newline)) continue;
        if (check(T::Indent) || check(T::Dedent)) {
            if (quiet == 0) diags.error(file, pos(), "unexpected indentation");
            advance();
            continue;
        }
        try {
            auto s = parseStatement();
            if (s) block->stmts.push_back(std::move(s));
            endStatement();
        } catch (ParseError&) {
            synchronize();
            if (diags.errorCount > 50) break;
        }
    }
    return block;
}

ExprP Parser::parseStandaloneExpr() {
    try {
        auto e = parseExpr();
        while (match(T::Newline)) {}
        if (!check(T::Eof)) failExpected("end of expression");
        return e;
    } catch (ParseError&) {
        return nullptr;
    }
}

// ------------------------------------------------------------------ blocks

std::unique_ptr<Block> Parser::parseStatementsUntilDedent(SrcPos p) {
    auto block = std::make_unique<Block>(p);
    while (!check(T::Dedent) && !check(T::Eof)) {
        if (match(T::Newline)) continue;
        if (check(T::Indent)) {
            if (quiet == 0) diags.error(file, pos(), "unexpected indentation");
            advance();
            continue;
        }
        try {
            auto s = parseStatement();
            if (s) block->stmts.push_back(std::move(s));
            endStatement();
        } catch (ParseError&) {
            if (quiet > 0) throw;
            synchronize();
            if (diags.errorCount > 50) throw;
        }
    }
    expect(T::Dedent, "end of block");
    return block;
}

std::unique_ptr<Block> Parser::parseBlock(bool allowEmpty) {
    SrcPos p = pos();
    if (match(T::Arrow)) {
        auto block = std::make_unique<Block>(p);
        int savedNoArrow = noArrow;
        noArrow = 0;
        block->stmts.push_back(parseStatement());
        noArrow = savedNoArrow;
        return block;
    }
    if (check(T::Newline) && peek(1).kind == T::Indent) {
        advance();
        advance();
        int savedNoArrow = noArrow;
        noArrow = 0;
        auto b = parseStatementsUntilDedent(p);
        noArrow = savedNoArrow;
        return b;
    }
    if (allowEmpty) return std::make_unique<Block>(p);
    if (check(T::Newline)) fail(peek(), "expected an indented block on the next line (or '-> statement' on this line)");
    failExpected("'->' or an indented block");
}

// ------------------------------------------------------------------ statements

StmtP Parser::parseStatement() {
    SrcPos p = pos();
    switch (peek().kind) {
        case T::KwFn:
            if (peek(1).kind == T::Ident) return parseFnDecl({}, false, false);
            break;
        case T::KwFork:
            if (peek(1).kind == T::KwFn && peek(2).kind == T::Ident) {
                advance();
                return parseFnDecl({}, true, false);
            }
            break;
        case T::KwPar:
            if (peek(1).kind == T::KwFn && peek(2).kind == T::Ident) {
                advance();
                return parseFnDecl({}, false, false, true);
            }
            break;
        case T::KwClass: return parseClass(false);
        case T::KwInterface: return parseInterface();
        case T::KwEnum: return parseEnum();
        case T::KwWhile: {
            advance();
            auto w = std::make_unique<While>(p);
            noArrow++;
            w->cond = parseExpr();
            noArrow--;
            w->body = parseBlock();
            return w;
        }
        case T::KwFor: return parseFor();
        case T::KwReturn: {
            advance();
            auto r = std::make_unique<Return>(p);
            if (!check(T::Newline) && !check(T::Dedent) && !check(T::Eof)) r->value = parseExpr();
            return r;
        }
        case T::KwBreak: advance(); return std::make_unique<Break>(p);
        case T::KwContinue: advance(); return std::make_unique<Continue>(p);
        case T::KwRetry: {
            advance();
            auto r = std::make_unique<Retry>(p);
            auto parseOne = [&]() {
                bool isDuration = (check(T::Int) || check(T::Float)) && !peek().text.empty() &&
                                  peek().text.back() == 's';
                ExprP e = parseExpr();
                if (isDuration) {
                    if (r->delay) fail(prev(), "retry takes at most one delay");
                    r->delay = std::move(e);
                } else {
                    if (r->count) fail(prev(), "retry takes at most one count");
                    r->count = std::move(e);
                }
            };
            if (!check(T::Newline) && !check(T::Dedent) && !check(T::Eof)) {
                parseOne();
                if (match(T::Comma)) parseOne();
            }
            return r;
        }
        case T::KwUse: return parseUse();
        case T::KwIn:
            if (peek(1).kind == T::Ident) return parseUseSelective();
            break;
        case T::KwVar: {
            const Token& kwVar = advance();
            const Token& name = expect(T::Ident, "a variable name");
            auto v = std::make_unique<VarDecl>(SrcPos{name.line, name.col}, name.text);
            if (match(T::Colon)) v->annot = parseType();
            if (match(T::Assign)) v->init = parseExpr();
            std::vector<TextEdit> fix{{kwVar.start, name.start, ""}};
            if (!v->init) fix.push_back({prev().end, prev().end, v->annot ? "? = nil" : " = nil"});
            deprecate(kwVar, "'var' isn't needed: assigning declares (x: int = 3 with a type)", std::move(fix));
            return v;
        }
        case T::KwWith: {
            advance();
            if (!(check(T::Ident) && peek().text == "tasks")) failExpected("'tasks' after 'with'");
            advance();
            auto w = std::make_unique<With>(p);
            w->body = parseBlock();
            return w;
        }
        case T::KwSelect: return parseSelect();
        case T::KwDefer: {
            advance();
            auto d = std::make_unique<Defer>(p);
            if (check(T::Newline) && peek(1).kind == T::Indent) {
                d->body = parseBlock();
            } else {
                d->body = std::make_unique<Block>(pos());
                d->body->stmts.push_back(parseStatement());
            }
            return d;
        }
        case T::KwTodo: {
            advance();
            auto t = std::make_unique<Todo>(p);
            if (check(T::Str)) t->message = advance().text;
            return t;
        }
        case T::At:
        case T::Dollar: return parseAttributed();
        case T::Ident:
            if (peek(1).kind == T::Star && !peek(1).spaceBefore &&
                (peek(2).kind == T::Assign || peek(2).kind == T::Colon)) {
                // limit* = 10: an exported module variable
                const Token& name = advance();
                advance();  // '*'
                SrcPos np{name.line, name.col};
                TypeP annot;
                if (match(T::Colon)) annot = parseType();
                expect(T::Assign, "'='");
                auto a = std::make_unique<Assign>(np, std::make_unique<Ident>(np, name.text), parseExpr());
                a->annot = std::move(annot);
                a->exported = true;
                return a;
            }
            break;
        default: break;
    }
    return parseSimpleStatement();
}

StmtP Parser::parseAttributed() {
    SrcPos p = pos();
    std::vector<std::string> attrs;
    while (true) {
        if (match(T::At)) {
            const Token& name = expect(T::Ident, "an attribute name after '@'");
            if (isRegionKind(name.text)) {
                // Memory region block: @arena, @frame, @cache assets ...
                auto r = std::make_unique<Region>(p);
                r->kind = name.text;
                if (isOldRegionName(name.text)) {
                    deprecate(name, "@" + name.text + " is @arena (they behaved the same); a comment can say what it's for",
                              {replaceTok(name, "arena")});
                    r->kind = "arena";
                }
                if (check(T::Ident)) {
                    const Token& rn = advance();
                    r->name = rn.text;
                    r->namePos = {rn.line, rn.col};
                    if (match(T::Assign)) r->init = parseExpr();
                }
                if ((r->kind == "cache" || r->kind == "dual") && r->name.empty())
                    fail(name, "@" + r->kind + " needs a name: @" + r->kind + " assets");
                while (match(T::Dollar)) expect(T::Ident, "a hint name after '$'");
                r->body = parseBlock();
                return r;
            }
            attrs.push_back(name.text);
        } else if (match(T::Dollar)) {
            const Token& name = expect(T::Ident, "a hint name after '$'");
            if (name.text == "hot") {
                deprecate(name, "$hot is $jit", {replaceTok(name, "jit")});
                attrs.push_back("$jit");
            } else {
                attrs.push_back("$" + name.text);
            }
        } else {
            break;
        }
        while (check(T::Newline) && (peek(1).kind == T::KwFn || peek(1).kind == T::At ||
                                     peek(1).kind == T::Dollar || peek(1).kind == T::KwFork || peek(1).kind == T::KwPar))
            advance();
    }
    if (check(T::KwFn)) return parseFnDecl(attrs, false, false);
    if (check(T::KwFork) && peek(1).kind == T::KwFn) {
        advance();
        return parseFnDecl(attrs, true, false);
    }
    if (check(T::KwPar) && peek(1).kind == T::KwFn) {
        advance();
        return parseFnDecl(attrs, false, false, true);
    }
    // $jit for / $jit while: compile the loop right away; other hints are ignored
    StmtP st = parseStatement();
    bool jit = std::find(attrs.begin(), attrs.end(), "$jit") != attrs.end();
    if (jit && st && st->kind == NK::For) static_cast<For*>(st.get())->jitHint = true;
    if (jit && st && st->kind == NK::While) static_cast<While*>(st.get())->jitHint = true;
    return st;
}

static bool isAssignTarget(Expr* e) {
    return e->kind == NK::Ident || e->kind == NK::Member || e->kind == NK::Index;
}

StmtP Parser::parseSimpleStatement() {
    SrcPos p = pos();
    ExprP e = parseExpr();
    auto compound = [&](T k) -> BinOp {
        switch (k) {
            case T::PlusAssign: return BinOp::Add;
            case T::MinusAssign: return BinOp::Sub;
            case T::StarAssign: return BinOp::Mul;
            case T::SlashAssign: return BinOp::Div;
            case T::PercentAssign: return BinOp::Mod;
            case T::StarStarAssign: return BinOp::Pow;
            default: return BinOp::None;
        }
    };
    if (check(T::Assign)) {
        const Token& eq = advance();
        if (!isAssignTarget(e.get()) || (e->kind == NK::Member && static_cast<Member*>(e.get())->isCall))
            fail(eq, "can't assign to this expression");
        ExprP v = parseExpr();
        return std::make_unique<Assign>(p, std::move(e), std::move(v));
    }
    BinOp op = compound(peek().kind);
    if (op != BinOp::None) {
        const Token& t = advance();
        if (!isAssignTarget(e.get())) fail(t, "can't assign to this expression");
        ExprP v = parseExpr();
        auto a = std::make_unique<Assign>(p, std::move(e), std::move(v));
        a->op = op;
        return a;
    }
    if (check(T::Colon) && e->kind == NK::Ident) {
        advance();
        TypeP t = parseType();
        expect(T::Assign, "'=' after the type annotation");
        ExprP v = parseExpr();
        auto a = std::make_unique<Assign>(p, std::move(e), std::move(v));
        a->annot = std::move(t);
        return a;
    }
    if (check(T::PlusPlus) || check(T::MinusMinus)) {
        bool inc = advance().kind == T::PlusPlus;
        if (!isAssignTarget(e.get())) fail(prev(), "can't increment this expression");
        return std::make_unique<IncDec>(p, std::move(e), inc);
    }
    if (check(T::Recv)) {
        advance();
        if (e->kind != NK::Ident) fail(prev(), "the left side of '<:' must be a variable name");
        auto r = std::make_unique<Recv>(p);
        r->target = std::move(e);
        r->chan = parseExpr();
        return r;
    }
    if (check(T::Comma) && isAssignTarget(e.get())) {
        auto d = std::make_unique<Destructure>(p);
        d->targets.push_back(std::move(e));
        while (match(T::Comma)) {
            ExprP t = parsePostfix(parsePrimary());
            if (!isAssignTarget(t.get())) fail(prev(), "can't assign to this expression");
            d->targets.push_back(std::move(t));
        }
        expect(T::Assign, "'=' after the assignment targets");
        ExprP first = parseExpr();
        if (check(T::Comma)) {
            auto tup = std::make_unique<TupleLit>(first->pos);
            tup->elems.push_back(std::move(first));
            while (match(T::Comma)) tup->elems.push_back(parseExpr());
            d->value = std::move(tup);
        } else {
            d->value = std::move(first);
        }
        return d;
    }
    if (e->kind == NK::Ident) static_cast<Ident*>(e.get())->statementCall = true;
    if (e->kind == NK::StrLit) static_cast<StrLit*>(e.get())->docString = true;
    return std::make_unique<ExprStmt>(p, std::move(e));
}

void Parser::parseParams(std::vector<Param>& params, bool allowSelf) {
    if (check(T::RParen)) return;
    do {
        if (check(T::RParen)) break;  // trailing comma
        Param prm;
        if (check(T::Ident) && peek().text == "mut" && peek(1).kind == T::Ident) {
            advance();
            prm.mut = true;
        }
        if (check(T::KwSelf) && allowSelf && params.empty()) {
            const Token& t = advance();
            prm.name = "self";
            prm.pos = {t.line, t.col};
        } else {
            const Token& t = expect(T::Ident, "a parameter name");
            prm.name = t.text;
            prm.pos = {t.line, t.col};
        }
        if (match(T::Colon)) prm.type = parseType();
        if (match(T::Assign)) prm.def = parseExpr();
        for (auto& other : params)
            if (other.name == prm.name) fail(prev(), "duplicate parameter '" + prm.name + "'");
        params.push_back(std::move(prm));
    } while (match(T::Comma));
}

// Docstring text without its common indentation and surrounding blank lines.
std::string cleanDocstring(const std::string& raw) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t nl = raw.find('\n', start);
        std::string l = raw.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    size_t indent = std::string::npos;
    for (size_t i = 1; i < lines.size(); i++) {
        size_t f = lines[i].find_first_not_of(" \t");
        if (f != std::string::npos) indent = std::min(indent, f);
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string l = lines[i];
        if (i > 0 && indent != std::string::npos && l.size() >= indent) l = l.substr(indent);
        if (i == 0) l.erase(0, l.find_first_not_of(" \t") == std::string::npos ? l.size() : l.find_first_not_of(" \t"));
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t')) l.pop_back();
        out += (i ? "\n" : "") + l;
    }
    size_t a = out.find_first_not_of('\n');
    if (a == std::string::npos) return "";
    size_t b = out.find_last_not_of('\n');
    return out.substr(a, b - a + 1);
}

std::unique_ptr<FnDecl> Parser::parseFnDecl(std::vector<std::string> attrs, bool isFork, bool inClass, bool isPar) {
    const Token& kw = expect(T::KwFn, "'fn'");
    auto fn = std::make_unique<FnDecl>(SrcPos{kw.line, kw.col});
    if (inClass && !check(T::Ident)) {
        // operator method
        const Token& op = peek();
        std::string opName;
        switch (op.kind) {
            case T::Plus: case T::Minus: case T::Star: case T::Slash: case T::Percent: case T::StarStar:
            case T::EqEq: case T::Lt:
            case T::PlusAssign: case T::MinusAssign: case T::StarAssign: case T::SlashAssign: case T::PercentAssign:
                opName = op.text;  // fn +=(o): a += b changes a in place
                advance();
                break;
            case T::LBracket:
                advance();
                expect(T::RBracket, "']' (operator [] is written fn [](index))");
                opName = "[]";
                if (match(T::Assign)) opName = "[]=";
                break;
            default: failExpected("a method name or an operator (+ - * / % ** == < [] += -= *= /= %=)");
        }
        fn->name = opName;
        fn->namePos = {op.line, op.col};
        fn->attrs = std::move(attrs);
        fn->isMethod = true;
        fn->exported = true;  // operators are always public
        expect(T::LParen, "'(' after the operator");
        parseParams(fn->params, false);
        expect(T::RParen, "')'");
        fn->hasParamList = true;
        if (fn->name == "-" && fn->params.empty()) fn->name = "neg";  // unary minus
        if (match(T::Colon)) {
            fn->ret = parseType();
        } else if (check(T::Arrow)) {
            const Token& before = prev();
            const Token& arrow = advance();
            deprecate(arrow, "a return type is written with ':' (fn f(x): int)", {{before.end, arrow.end, ":"}});
            fn->ret = parseType();
        }
        SrcPos bp = pos();
        if (match(T::Assign)) {
            auto body = std::make_unique<Block>(bp);
            SrcPos ep = pos();
            body->stmts.push_back(std::make_unique<ExprStmt>(ep, parseExpr()));
            fn->body = std::move(body);
        } else {
            fn->body = parseBlock(true);
        }
        fn->endPos = {prev().line, prev().col};
        return fn;
    }
    const Token& name = expect(T::Ident, "a function name");
    fn->name = name.text;
    fn->namePos = {name.line, name.col};
    fn->attrs = std::move(attrs);
    fn->isFork = isFork;
    fn->isPar = isPar;
    fn->isMethod = inClass;
    if (match(T::Star)) fn->exported = true;
    if (check(T::LBracket) && !peek().spaceBefore) {
        // fn first[T](xs: list[T]): T
        advance();
        do {
            const Token& tp = expect(T::Ident, "a type parameter (a capitalized name such as T)");
            if (!std::isupper((unsigned char)tp.text[0])) fail(tp, "type parameters start with a capital letter (T, Key)");
            fn->typeParams.push_back(tp.text);
        } while (match(T::Comma));
        expect(T::RBracket, "']' after the type parameters");
    }
    typeParamScopes.push_back(fn->typeParams);
    struct PopScope {
        std::vector<std::vector<std::string>>& s;
        ~PopScope() { s.pop_back(); }
    } popScope{typeParamScopes};
    if (check(T::StarAssign)) {
        // fn name*= expr  (export marker glued to a one-line body)
        advance();
        fn->exported = true;
        SrcPos bp = pos();
        auto body = std::make_unique<Block>(bp);
        body->stmts.push_back(std::make_unique<ExprStmt>(bp, parseExpr()));
        fn->body = std::move(body);
        fn->endPos = {prev().line, prev().col};
        return fn;
    }
    if (match(T::LParen)) {
        fn->hasParamList = true;
        parseParams(fn->params, opts.prelude);
        expect(T::RParen, "')' after the parameters");
    }
    if (check(T::StarAssign)) {
        const Token& sa = advance();
        if (!fn->exported)
            deprecate(sa, "the export '*' goes right after the name: fn " + fn->name + "*(...)",
                      {{name.end, name.end, "*"}, replaceTok(sa, " =")});
        fn->exported = true;
        if (!fn->params.empty() && fn->params[0].name == "self") fn->selfFirst = true;
        SrcPos bp = pos();
        auto body = std::make_unique<Block>(bp);
        body->stmts.push_back(std::make_unique<ExprStmt>(bp, parseExpr()));
        fn->body = std::move(body);
        fn->endPos = {prev().line, prev().col};
        return fn;
    }
    if (check(T::Star)) {
        const Token& st = advance();
        if (!fn->exported)
            deprecate(st, "the export '*' goes right after the name: fn " + fn->name + "*(...)",
                      {{st.start, st.end, ""}, {name.end, name.end, "*"}});
        fn->exported = true;
    }
    if (!fn->params.empty() && fn->params[0].name == "self") fn->selfFirst = true;
    if (check(T::Colon)) {
        advance();
        fn->ret = parseType();
        if (!check(T::Newline) && !check(T::Assign) && !check(T::Bang) && !check(T::Eof) && !check(T::Dedent))
            fail(peek(), "after ':' comes the return type; write a one-line body with '=' (fn f(x): int = x * 2)");
    } else if (check(T::Arrow)) {
        const Token& before = prev();
        const Token& arrow = advance();
        deprecate(arrow, "a return type is written with ':' (fn f(x): int)", {{before.end, arrow.end, ":"}});
        fn->ret = parseType();
        if (!check(T::Newline) && !check(T::Assign) && !check(T::Bang) && !check(T::Eof) && !check(T::Dedent))
            fail(peek(), "after the return type comes '=' and a one-line body, or an indented body");
    }
    if (check(T::Bang)) {
        advance();
        auto res = std::make_unique<TypeExpr>();
        res->k = TypeExpr::Result;
        res->pos = fn->ret ? fn->ret->pos : SrcPos{prev().line, prev().col};
        if (fn->ret) {
            res->args.push_back(std::move(fn->ret));
        } else {
            auto any = std::make_unique<TypeExpr>();
            any->name = "any";
            res->args.push_back(std::move(any));
        }
        res->ret = parseType();
        fn->ret = std::move(res);
    }
    SrcPos bp = pos();
    if (match(T::Assign)) {
        auto body = std::make_unique<Block>(bp);
        SrcPos ep = pos();
        body->stmts.push_back(std::make_unique<ExprStmt>(ep, parseExpr()));
        fn->body = std::move(body);
    } else {
        fn->body = parseBlock(true);
        fn->hasBody = !fn->body->stmts.empty() || prev().kind == T::Dedent;
    }
    fn->endPos = {prev().line, prev().col};
    if (fn->body && !fn->body->stmts.empty() && fn->body->stmts[0]->kind == NK::ExprStmt) {
        Expr* first = static_cast<ExprStmt*>(fn->body->stmts[0].get())->expr.get();
        if (first->kind == NK::StrLit && (fn->body->stmts.size() > 1 || static_cast<StrLit*>(first)->docString))
            fn->doc = cleanDocstring(static_cast<StrLit*>(first)->value);
    }
    return fn;
}

StmtP Parser::parseClass(bool) {
    const Token& kw = expect(T::KwClass, "'class'");
    auto c = std::make_unique<ClassDecl>(SrcPos{kw.line, kw.col});
    const Token& name = expect(T::Ident, "a class name");
    c->name = name.text;
    c->namePos = {name.line, name.col};
    if (match(T::Star)) c->exported = true;
    auto lateStar = [&]() {
        if (!check(T::Star)) return;
        const Token& st = advance();
        if (!c->exported)
            deprecate(st, "the export '*' goes right after the name: class " + c->name + "*",
                      {{st.start, st.end, ""}, {name.end, name.end, "*"}});
        c->exported = true;
    };
    if (match(T::LBracket)) {
        // generic class: class Box[T], class Cache[Key, Value]
        do {
            const Token& tp = expect(T::Ident, "a type parameter (a capitalized name such as T)");
            if (!std::isupper((unsigned char)tp.text[0])) fail(tp, "type parameters start with a capital letter (T, Key)");
            c->typeParams.push_back(tp.text);
        } while (match(T::Comma));
        expect(T::RBracket, "']' after the type parameters");
    }
    typeParamScopes.push_back(c->typeParams);
    struct PopScope {
        std::vector<std::vector<std::string>>& s;
        ~PopScope() { s.pop_back(); }
    } popScope{typeParamScopes};
    lateStar();
    if (match(T::LParen)) {
        c->hasCtorParams = true;
        parseParams(c->ctorParams, false);
        expect(T::RParen, "')' after the constructor parameters");
    }
    lateStar();
    if (match(T::KwIs)) {
        do {
            const Token& in = expect(T::Ident, "an interface name after 'is'");
            c->interfaces.push_back({in.text, SrcPos{in.line, in.col}});
        } while (match(T::Comma));
    }
    if (!(check(T::Newline) && peek(1).kind == T::Indent)) return c;  // empty class
    advance();
    advance();
    while (!check(T::Dedent) && !check(T::Eof)) {
        if (match(T::Newline)) continue;
        try {
            std::vector<std::string> attrs;
            bool isFork = false;
            while (check(T::At) || check(T::Dollar)) {
                bool dollar = advance().kind == T::Dollar;
                attrs.push_back((dollar ? "$" : "") + expect(T::Ident, "an attribute name").text);
                while (match(T::Newline)) {}
            }
            if (check(T::KwFork) && peek(1).kind == T::KwFn) {
                advance();
                isFork = true;
            }
            if (check(T::KwFn)) {
                c->methods.push_back(parseFnDecl(attrs, isFork, true));
            } else if (check(T::Ident)) {
                const Token& fname = advance();
                FieldDecl fd;
                fd.name = fname.text;
                fd.pos = {fname.line, fname.col};
                if (match(T::Colon)) fd.type = parseType();
                if (match(T::Assign)) fd.def = parseExpr();
                c->fields.push_back(std::move(fd));
            } else if (check(T::Str)) {
                const Token& ds = advance();  // docstring
                if (c->doc.empty()) c->doc = cleanDocstring(ds.text);
            } else {
                failExpected("a field or 'fn' method in the class body");
            }
            endStatement();
        } catch (ParseError&) {
            if (quiet > 0) throw;
            synchronize();
        }
    }
    expect(T::Dedent, "end of class body");
    return c;
}

StmtP Parser::parseInterface() {
    const Token& kw = expect(T::KwInterface, "'interface'");
    auto d = std::make_unique<InterfaceDecl>(SrcPos{kw.line, kw.col});
    const Token& name = expect(T::Ident, "an interface name");
    d->name = name.text;
    d->namePos = {name.line, name.col};
    if (match(T::Star)) d->exported = true;
    if (match(T::KwIs)) {
        do {
            const Token& b = expect(T::Ident, "an interface name after 'is'");
            d->bases.push_back({b.text, SrcPos{b.line, b.col}});
        } while (match(T::Comma));
    }
    if (!(check(T::Newline) && peek(1).kind == T::Indent)) return d;
    advance();
    advance();
    while (!check(T::Dedent) && !check(T::Eof)) {
        if (match(T::Newline)) continue;
        try {
            if (check(T::Str)) {
                const Token& ds = advance();
                if (d->doc.empty()) d->doc = cleanDocstring(ds.text);
            } else {
                auto m = parseFnDecl({}, false, true);
                m->exported = true;  // interface methods are public
                d->methods.push_back(std::move(m));
            }
            endStatement();
        } catch (ParseError&) {
            if (quiet > 0) throw;
            synchronize();
        }
    }
    expect(T::Dedent, "end of interface");
    return d;
}

StmtP Parser::parseEnum() {
    const Token& kw = expect(T::KwEnum, "'enum'");
    auto e = std::make_unique<EnumDecl>(SrcPos{kw.line, kw.col});
    const Token& name = expect(T::Ident, "an enum name");
    e->name = name.text;
    e->namePos = {name.line, name.col};
    if (match(T::Star)) e->exported = true;
    expect(T::Newline, "end of line after the enum name");
    expect(T::Indent, "the enum's variants, indented");
    while (!check(T::Dedent) && !check(T::Eof)) {
        if (match(T::Newline)) continue;
        try {
            if (check(T::KwFn)) {
                e->methods.push_back(parseFnDecl({}, false, true));
            } else if (check(T::Str)) {
                const Token& ds = advance();
                if (e->doc.empty()) e->doc = cleanDocstring(ds.text);
            } else {
                const Token& vn = expect(T::Ident, "a variant name");
                EnumVariant v;
                v.name = vn.text;
                v.pos = {vn.line, vn.col};
                v.cls = std::make_unique<ClassDecl>(SrcPos{vn.line, vn.col});
                v.cls->name = e->name + "." + vn.text;
                v.cls->namePos = {vn.line, vn.col};
                v.cls->exported = true;
                if (match(T::LParen)) {
                    v.unit = false;
                    v.cls->hasCtorParams = true;
                    parseParams(v.cls->ctorParams, false);
                    expect(T::RParen, "')'");
                }
                for (auto& other : e->variants)
                    if (other.name == v.name) fail(vn, "variant '" + v.name + "' is defined twice");
                e->variants.push_back(std::move(v));
            }
            endStatement();
        } catch (ParseError&) {
            if (quiet > 0) throw;
            synchronize();
        }
    }
    expect(T::Dedent, "end of enum");
    for (auto& m : e->methods) m->exported = true;  // shared methods are public
    return e;
}

StmtP Parser::parseUse() {
    const Token& kw = expect(T::KwUse, "'use'");
    auto u = std::make_unique<Use>(SrcPos{kw.line, kw.col});
    u->path.push_back(expect(T::Ident, "a module name after 'use'").text);
    while (match(T::Dot)) u->path.push_back(expect(T::Ident, "a module name after '.'").text);
    if (match(T::KwAs)) u->alias = expect(T::Ident, "a name after 'as'").text;
    return u;
}

StmtP Parser::parseUseSelective() {
    const Token& kw = expect(T::KwIn, "'in'");
    auto u = std::make_unique<Use>(SrcPos{kw.line, kw.col});
    u->selective = true;
    u->path.push_back(expect(T::Ident, "a module name after 'in'").text);
    while (match(T::Dot)) u->path.push_back(expect(T::Ident, "a module name after '.'").text);
    expect(T::KwUse, "'use' (in module use name, ...)");
    do {
        const Token& n = expect(T::Ident, "a name to import");
        UseItem item;
        item.name = n.text;
        item.pos = {n.line, n.col};
        if (match(T::KwAs)) item.alias = expect(T::Ident, "a name after 'as'").text;
        u->items.push_back(item);
    } while (match(T::Comma));
    return u;
}

StmtP Parser::parseFor() {
    const Token& kw = expect(T::KwFor, "'for'");
    SrcPos p{kw.line, kw.col};
    std::vector<LoopVar> vars;
    bool paren = match(T::LParen);
    size_t namesStart = peek().start;
    bool unpackItem = false;
    do {
        if (!paren && vars.size() == 1 && check(T::LParen)) {
            // for i, (name, score) in results
            advance();
            do {
                const Token& n = expect(T::Ident, "a name to unpack the item into");
                vars.push_back({n.text, {n.line, n.col}, nullptr});
            } while (match(T::Comma));
            expect(T::RParen, "')'");
            unpackItem = true;
            break;
        }
        const Token& n = expect(T::Ident, "a loop variable name");
        vars.push_back({n.text, {n.line, n.col}, nullptr});
    } while (match(T::Comma));
    size_t namesEnd = prev().end;
    if (paren) expect(T::RParen, "')'");
    if (check(T::Recv)) {
        if (vars.size() != 1) fail(peek(), "'<:' receives into exactly one variable");
        auto fr = std::make_unique<ForRecv>(p);
        advance();
        noArrow++;
        fr->chans.push_back(parseExpr());
        fr->vars.push_back(vars[0]);
        while (match(T::Comma)) {
            const Token& n = expect(T::Ident, "a variable name");
            fr->vars.push_back({n.text, {n.line, n.col}, nullptr});
            expect(T::Recv, "'<:'");
            fr->chans.push_back(parseExpr());
        }
        noArrow--;
        fr->body = parseBlock();
        return fr;
    }
    expect(T::KwIn, "'in' or '<:' after the loop variable");
    auto f = std::make_unique<For>(p);
    f->vars = std::move(vars);
    f->unpackItem = unpackItem;
    f->destructure = !unpackItem && (paren || f->vars.size() > 2);
    f->fixStart = namesStart;
    f->fixEnd = namesEnd;
    noArrow++;
    f->iter = parseExpr();
    if (f->vars.size() == 2 && !paren && f->iter->kind == NK::Member &&
        static_cast<Member*>(f->iter.get())->name == "indexed" && !static_cast<Member*>(f->iter.get())->isCall) {
        // for i, x in xs.indexed: two names already give the position and the item
        const Token& dot = toks[i - 2];
        deprecate(prev(), "for i, x in xs gives the position and the item; .indexed isn't needed",
                  {{dot.start, prev().end, ""}});
        f->iter = std::move(static_cast<Member*>(f->iter.get())->object);
    }
    if (f->iter->kind == NK::Range && static_cast<Range*>(f->iter.get())->step) {
        f->step = std::move(static_cast<Range*>(f->iter.get())->step);  // for i in 0..10 by 2
    } else if (check(T::Comma)) {
        const Token& comma = advance();
        deprecate(comma, "a step is written with 'by' (for i in 0..10 by 2)", {{comma.start, peek().start, " by "}});
        f->step = parseExpr();
    }
    noArrow--;
    f->body = parseBlock();
    return f;
}

// ------------------------------------------------------------------ expressions

ExprP Parser::parseExpr() { return parseSend(); }

ExprP Parser::parseSend() {
    ExprP left = parseCatch();
    while (!blockEnded() && check(T::Send)) {
        SrcPos p = pos();
        advance();
        ExprP right = parseCatch();
        left = std::make_unique<Binary>(p, BinOp::Send, std::move(left), std::move(right));
    }
    return left;
}

ExprP Parser::parseCatch() {
    ExprP e = parseOr();
    if (!blockEnded() && check(T::Bang)) {
        SrcPos p = pos();
        advance();
        auto c = std::make_unique<Catch>(p);
        c->expr = std::move(e);
        if (check(T::Ident)) {
            const Token& n = advance();
            c->errName = n.text;
            c->errPos = {n.line, n.col};
        }
        c->handler = parseBlock();
        return c;
    }
    return e;
}

ExprP Parser::parseOr() {
    ExprP left = parseAnd();
    while (!blockEnded() && (check(T::KwOr) || check(T::PipePipe))) {
        SrcPos p = pos();
        const Token& op = advance();
        if (op.kind == T::PipePipe) deprecate(op, "'||' is 'or'", {replaceTok(op, spaced(op, "or"))});
        left = std::make_unique<Binary>(p, BinOp::Or, std::move(left), parseAnd());
    }
    return left;
}

ExprP Parser::parseAnd() {
    ExprP left = parseNot();
    while (!blockEnded() && (check(T::KwAnd) || check(T::AmpAmp))) {
        SrcPos p = pos();
        const Token& op = advance();
        if (op.kind == T::AmpAmp) deprecate(op, "'&&' is 'and'", {replaceTok(op, spaced(op, "and"))});
        left = std::make_unique<Binary>(p, BinOp::And, std::move(left), parseNot());
    }
    return left;
}

ExprP Parser::parseNot() {
    if (check(T::KwNot)) {
        SrcPos p = pos();
        advance();
        return std::make_unique<Unary>(p, UnOp::Not, parseNot());
    }
    return parseComparison();
}

ExprP Parser::parseComparison() {
    ExprP left = parseRange();
    while (true) {
        SrcPos p = pos();
        BinOp op = BinOp::None;
        switch (peek().kind) {
            case T::EqEq: op = BinOp::Eq; break;
            case T::NotEq: op = BinOp::Ne; break;
            case T::Lt: op = BinOp::Lt; break;
            case T::Gt: op = BinOp::Gt; break;
            case T::LtEq: op = BinOp::Le; break;
            case T::GtEq: op = BinOp::Ge; break;
            case T::EqEqEq: op = BinOp::Is; break;
            case T::NotEqEq: op = BinOp::IsNot; break;
            case T::KwIn: op = BinOp::In; break;
            case T::KwIs:
                op = peek(1).kind == T::KwNot ? BinOp::IsNot : BinOp::Is;
                if (op == BinOp::IsNot) advance();
                break;
            case T::KwNot:
                if (peek(1).kind == T::KwIn) {
                    advance();
                    op = BinOp::NotIn;
                }
                break;
            default: break;
        }
        if (op == BinOp::None || blockEnded()) return left;
        const Token& opTok = advance();
        if (opTok.kind == T::EqEqEq) deprecate(opTok, "'===' is 'is'", {replaceTok(opTok, spaced(opTok, "is"))});
        if (opTok.kind == T::NotEqEq) deprecate(opTok, "'!==' is 'is not'", {replaceTok(opTok, spaced(opTok, "is not"))});
        left = std::make_unique<Binary>(p, op, std::move(left), parseRange());
    }
}

static bool isBy(const Token& t) { return t.kind == T::Ident && t.text == "by"; }

ExprP Parser::parseRange() {
    auto rangeOp = [&]() {
        const Token& op = advance();
        if (op.kind == T::DotDotLess) deprecate(op, "'..<' is '..' (the end isn't included)", {replaceTok(op, "..")});
        return op.kind == T::DotDotEq;
    };
    auto withStep = [&](std::unique_ptr<Range> r) -> ExprP {
        if (isBy(peek())) {
            advance();
            r->step = parseBitOr();  // 0..100 by 5
        }
        return r;
    };
    // xs[..3], xs[2..], xs[.. by -1]: open ends only inside an index
    if (sliceContext > 0 && (check(T::DotDot) || check(T::DotDotLess) || check(T::DotDotEq))) {
        SrcPos p = pos();
        bool inclusive = rangeOp();
        ExprP right = check(T::RBracket) || check(T::Comma) || isBy(peek()) ? nullptr : parseBitOr();
        return withStep(std::make_unique<Range>(p, nullptr, std::move(right), inclusive));
    }
    ExprP left = parseBitOr();
    if (!blockEnded() && (check(T::DotDot) || check(T::DotDotLess) || check(T::DotDotEq))) {
        SrcPos p = pos();
        bool inclusive = rangeOp();
        if (sliceContext > 0 && (check(T::RBracket) || check(T::Comma) || isBy(peek())))
            return withStep(std::make_unique<Range>(p, std::move(left), nullptr, inclusive));
        ExprP right = parseBitOr();
        return withStep(std::make_unique<Range>(p, std::move(left), std::move(right), inclusive));
    }
    return left;
}

ExprP Parser::parseBitOr() {
    ExprP left = parseBitXor();
    while (!blockEnded() && check(T::Pipe)) {
        SrcPos p = pos();
        advance();
        left = std::make_unique<Binary>(p, BinOp::BitOr, std::move(left), parseBitXor());
    }
    return left;
}

ExprP Parser::parseBitXor() {
    ExprP left = parseBitAnd();
    while (!blockEnded() && check(T::Caret)) {
        SrcPos p = pos();
        advance();
        left = std::make_unique<Binary>(p, BinOp::BitXor, std::move(left), parseBitAnd());
    }
    return left;
}

ExprP Parser::parseBitAnd() {
    ExprP left = parseShift();
    while (!blockEnded() && check(T::Amp)) {
        SrcPos p = pos();
        advance();
        left = std::make_unique<Binary>(p, BinOp::BitAnd, std::move(left), parseShift());
    }
    return left;
}

ExprP Parser::parseShift() {
    ExprP left = parseAdd();
    while (!blockEnded() && (check(T::Shl) || check(T::Shr))) {
        SrcPos p = pos();
        BinOp op = advance().kind == T::Shl ? BinOp::Shl : BinOp::Shr;
        left = std::make_unique<Binary>(p, op, std::move(left), parseAdd());
    }
    return left;
}

ExprP Parser::parseAdd() {
    ExprP left = parseMul();
    while (!blockEnded() && (check(T::Plus) || check(T::Minus))) {
        SrcPos p = pos();
        BinOp op = advance().kind == T::Plus ? BinOp::Add : BinOp::Sub;
        left = std::make_unique<Binary>(p, op, std::move(left), parseMul());
    }
    return left;
}

ExprP Parser::parseMul() {
    ExprP left = parseUnary();
    while (true) {
        BinOp op;
        if (blockEnded()) return left;
        switch (peek().kind) {
            case T::Star: op = BinOp::Mul; break;
            case T::Slash: op = BinOp::Div; break;
            case T::Percent: op = BinOp::Mod; break;
            case T::SlashSlash: op = BinOp::IntDiv; break;  // floor division
            case T::TildeSlash: op = BinOp::IntDiv; break;
            default: return left;
        }
        SrcPos p = pos();
        const Token& opTok = advance();
        if (opTok.kind == T::TildeSlash) deprecate(opTok, "'~/' is '//' (integer division)", {replaceTok(opTok, "//")});
        left = std::make_unique<Binary>(p, op, std::move(left), parseUnary());
    }
}

ExprP Parser::parseUnary() {
    SrcPos p = pos();
    if (check(T::Amp)) {
        // &tick, &obj.method: the function itself (a function declared without
        // parentheses is otherwise called wherever its name is used)
        const Token& amp = advance();
        ExprP operand = parsePostfix(parsePrimary());
        if (operand->kind == NK::Ident)
            static_cast<Ident*>(operand.get())->ref = true;
        else if (operand->kind == NK::Member && !static_cast<Member*>(operand.get())->isCall)
            static_cast<Member*>(operand.get())->ref = true;
        else
            fail(amp, "'&' takes a function's name: &tick or &obj.method");
        return operand;
    }
    if (check(T::Minus)) {
        // Unary minus binds looser than ** and '.', as in maths: -2 ** 2 is -4,
        // -5.abs is -(5.abs). A plain number folds into a negative literal.
        const Token& minus = advance();
        const Token& next = peek();
        bool numberFirst = next.kind == T::Int || next.kind == T::Float;
        ExprP operand = parseUnary();
        if (operand->kind == NK::IntLit) {
            auto* il = static_cast<IntLit*>(operand.get());
            il->value = (int64_t)(0ULL - (uint64_t)il->value);
            operand->pos = p;
            return operand;
        }
        if (operand->kind == NK::FloatLit) {
            static_cast<FloatLit*>(operand.get())->value = -static_cast<FloatLit*>(operand.get())->value;
            operand->pos = p;
            return operand;
        }
        // Before 0.6, -5.abs was (-5).abs and -2 ** 2 was (-2) ** 2: the fix keeps that meaning.
        if (numberFirst && operand->kind == NK::Member)
            deprecate(minus, "-" + next.text + "." + static_cast<Member*>(operand.get())->name + " now means -(" + next.text +
                                 "." + static_cast<Member*>(operand.get())->name + "); write (-" + next.text +
                                 ")." + static_cast<Member*>(operand.get())->name + " for the method on -" + next.text,
                      {{minus.start, minus.start, "("}, {next.end, next.end, ")"}});
        if (numberFirst && operand->kind == NK::Binary && static_cast<Binary*>(operand.get())->op == BinOp::Pow)
            deprecate(minus, "-" + next.text + " ** x now means -(" + next.text + " ** x), as in maths; write (-" +
                                 next.text + ") ** x to raise -" + next.text,
                      {{minus.start, minus.start, "("}, {next.end, next.end, ")"}});
        return std::make_unique<Unary>(p, UnOp::Neg, std::move(operand));
    }
    if (check(T::Bang)) {
        const Token& bang = advance();
        // `!x` is `not x`; parentheses keep the meaning when an operator is next to it
        auto tight = [](T k) {
            switch (k) {
                case T::Star: case T::Slash: case T::Percent: case T::SlashSlash: case T::TildeSlash:
                case T::Plus: case T::Minus: case T::Shl: case T::Shr: case T::Amp: case T::Pipe:
                case T::Caret: case T::DotDot: case T::DotDotLess: case T::DotDotEq: case T::EqEq:
                case T::NotEq: case T::Lt: case T::Gt: case T::LtEq: case T::GtEq: case T::EqEqEq:
                case T::NotEqEq: case T::KwIn: case T::KwIs: case T::StarStar: case T::Tilde: case T::Bang:
                    return true;
                default: return false;
            }
        };
        bool before = i >= 2 && tight(toks[i - 2].kind);
        ExprP operand = parseUnary();
        bool after = tight(peek().kind) || (check(T::KwNot) && peek(1).kind == T::KwIn);
        if (before || after)
            deprecate(bang, "'!x' is 'not x'", {{bang.start, bang.end, "(not "}, {prev().end, prev().end, ")"}});
        else
            deprecate(bang, "'!x' is 'not x'", {replaceTok(bang, "not ")});
        return std::make_unique<Unary>(p, UnOp::Not, std::move(operand));
    }
    if (check(T::Tilde)) {
        advance();
        return std::make_unique<Unary>(p, UnOp::BitNot, parseUnary());
    }
    return parsePower(parsePostfix(parsePrimary()));
}

ExprP Parser::parsePower(ExprP base) {
    if (!blockEnded() && check(T::StarStar)) {
        SrcPos p = pos();
        advance();
        return std::make_unique<Binary>(p, BinOp::Pow, std::move(base), parseUnary());
    }
    return base;
}

ExprP Parser::parseArg() {
    if (check(T::Arrow)) return parseArrowLambda(false);
    return wrapPlaceholders(parseExpr());
}

void Parser::parseArgs(std::vector<ExprP>& args, bool methodCall) {
    expect(T::LParen, "'('");
    int savedNoArrow = noArrow;
    noArrow = 0;
    callStack.push_back(methodCall);
    try {
        bool sawNamed = false;
        while (!check(T::RParen)) {
            if (check(T::Ident) && peek(1).kind == T::Colon) {
                const Token& nameTok = advance();
                advance();  // ':'
                SrcPos np{nameTok.line, nameTok.col};
                for (auto& prev : args)
                    if (prev->kind == NK::NamedArg && static_cast<NamedArg*>(prev.get())->name == nameTok.text)
                        fail(nameTok, "the argument '" + nameTok.text + "' is given twice");
                args.push_back(std::make_unique<NamedArg>(np, nameTok.text, parseArg()));
                sawNamed = true;
            } else {
                if (sawNamed) fail(peek(), "positional arguments go before named ones (name: value)");
                args.push_back(parseArg());
            }
            if (!match(T::Comma)) break;
        }
    } catch (...) {
        callStack.pop_back();
        noArrow = savedNoArrow;
        throw;
    }
    callStack.pop_back();
    noArrow = savedNoArrow;
    expect(T::RParen, "')' after the arguments");
}

// A call argument that mentions placeholders (_ or _name) becomes a lambda:
// list.map(_ * 2)  =>  list.map(fn(_) -> _ * 2)
// The lambda forms at the argument of the nearest *method* call containing the
// placeholder; plain calls are transparent, so data.map(Point(_.x, 0)) makes
// the lambda for .map. Without an enclosing method call, the nearest call's
// argument becomes the lambda (apply(_ * 3, 7)). A bare placeholder argument
// (print(_)) is always left for an enclosing call.
ExprP Parser::wrapPlaceholders(ExprP e) {
    if (arrowBody > 0) return e;
    if (e->kind == NK::Ident && static_cast<Ident*>(e.get())->isPlaceholder()) return e;
    std::vector<std::string> names;
    collectPlaceholders(e.get(), names);
    if (names.empty()) return e;
    if (!callStack.empty() && !callStack.back()) {
        for (size_t k = 0; k + 1 < callStack.size(); k++)
            if (callStack[k]) return e;  // an outer method call will take it
    }
    std::sort(names.begin(), names.end());
    SrcPos p = e->pos;
    auto fn = std::make_unique<FnDecl>(p);
    fn->name = "<lambda>";
    fn->isLambda = true;
    fn->lenientArity = true;
    for (auto& n : names) {
        Param prm;
        prm.name = n;
        prm.pos = p;
        fn->params.push_back(std::move(prm));
    }
    fn->body = std::make_unique<Block>(p);
    fn->body->stmts.push_back(std::make_unique<ExprStmt>(p, std::move(e)));
    return std::make_unique<Lambda>(p, std::move(fn));
}

// `-> expr` lambda (argument position) or trailing `-> stmt` / `->` + block.
ExprP Parser::parseArrowLambda(bool statementBody) {
    SrcPos p = pos();
    expect(T::Arrow, "'->'");
    auto fn = std::make_unique<FnDecl>(p);
    fn->name = "<lambda>";
    fn->isLambda = true;
    fn->lenientArity = true;
    arrowBody++;
    int savedNoArrow = noArrow;
    noArrow = 0;
    try {
        if (check(T::Newline) && peek(1).kind == T::Indent) {
            advance();
            advance();
            fn->body = parseStatementsUntilDedent(p);
        } else {
            fn->body = std::make_unique<Block>(p);
            if (statementBody) {
                fn->body->stmts.push_back(parseStatement());
            } else {
                SrcPos ep = pos();
                fn->body->stmts.push_back(std::make_unique<ExprStmt>(ep, parseExpr()));
            }
        }
    } catch (...) {
        arrowBody--;
        noArrow = savedNoArrow;
        throw;
    }
    noArrow = savedNoArrow;
    arrowBody--;
    std::vector<std::string> names;
    collectPlaceholders(fn->body.get(), names);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        Param prm;
        prm.name = n;
        prm.pos = p;
        fn->params.push_back(std::move(prm));
    }
    return std::make_unique<Lambda>(p, std::move(fn));
}

ExprP Parser::parsePostfix(ExprP e) {
    while (true) {
        if (blockEnded()) return e;
        if (check(T::LParen)) {
            SrcPos p = pos();
            if (e->kind == NK::Member && !static_cast<Member*>(e.get())->isCall) {
                auto* m = static_cast<Member*>(e.get());
                m->isCall = true;
                parseArgs(m->args, true);
            } else {
                if (e->kind == NK::Ident) static_cast<Ident*>(e.get())->callee = true;
                auto c = std::make_unique<Call>(p, std::move(e));
                c->lparen = peek().start;
                parseArgs(c->args, false);
                c->rparen = prev().start;
                e = std::move(c);
            }
        } else if (check(T::Dot)) {
            SrcPos p = pos();
            advance();
            const Token& name = check(T::KwSelf) ? advance() : expect(T::Ident, "a member name after '.'");
            e = std::make_unique<Member>(p, std::move(e), name.text, SrcPos{name.line, name.col});
            static_cast<Member*>(e.get())->nameEnd = name.end;
        } else if (check(T::LBracket)) {
            SrcPos p = pos();
            advance();
            int savedNoArrow = noArrow;
            noArrow = 0;
            sliceContext++;
            ExprP idx;
            try {
                idx = parseExpr();
                if (check(T::Comma) && idx->kind == NK::Range && !static_cast<Range*>(idx.get())->step) {
                    // xs[0..10, 2]: the old way to write a step
                    const Token& comma = advance();
                    deprecate(comma, "a step is written with 'by' (xs[0..10 by 2])", {{comma.start, peek().start, " by "}});
                    static_cast<Range*>(idx.get())->step = parseExpr();
                } else if (check(T::Comma)) {
                    // m[i, j] is m[(i, j)]: matrices, maps with tuple keys
                    auto tup = std::make_unique<TupleLit>(idx->pos);
                    tup->elems.push_back(std::move(idx));
                    while (match(T::Comma)) {
                        if (check(T::RBracket)) break;
                        tup->elems.push_back(parseExpr());
                    }
                    idx = std::move(tup);
                }
            } catch (...) {
                sliceContext--;
                noArrow = savedNoArrow;
                throw;
            }
            sliceContext--;
            noArrow = savedNoArrow;
            expect(T::RBracket, "']'");
            e = std::make_unique<Index>(p, std::move(e), std::move(idx));
        } else if (check(T::Question)) {
            SrcPos p = pos();
            advance();
            e = std::make_unique<Try>(p, std::move(e));
        } else if (check(T::Arrow) && noArrow == 0 && (e->kind == NK::Member || e->kind == NK::Call)) {
            // trailing lambda: list.each -> print(_)
            ExprP lam = parseArrowLambda(true);
            if (e->kind == NK::Member) {
                auto* m = static_cast<Member*>(e.get());
                m->isCall = true;
                m->args.push_back(std::move(lam));
            } else {
                static_cast<Call*>(e.get())->args.push_back(std::move(lam));
            }
            return e;
        } else {
            return e;
        }
    }
}

ExprP Parser::parseNumber(const Token& t) {
    SrcPos p{t.line, t.col};
    std::string text = t.text;
    double scale = 0;
    if (text.size() > 2 && text.compare(text.size() - 2, 2, "ms") == 0) {
        scale = 1;
        text.resize(text.size() - 2);
    } else if (!text.empty() && text.back() == 's') {
        scale = 1000;
        text.resize(text.size() - 1);
    }
    if (t.kind == T::Float) {
        double v = std::strtod(text.c_str(), nullptr);
        if (scale) return std::make_unique<IntLit>(p, (int64_t)std::llround(v * scale));
        return std::make_unique<FloatLit>(p, v);
    }
    int base = 10;
    std::string digits = text;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'b')) {
        base = text[1] == 'x' ? 16 : 2;
        digits = text.substr(2);
    }
    errno = 0;
    unsigned long long v = std::strtoull(digits.c_str(), nullptr, base);
    if (errno == ERANGE || (base == 10 && v > 9223372036854775808ULL)) {
        if (quiet == 0) diags.error(file, p, "integer literal is too large (max 9223372036854775807)");
        v = 0;
    }
    int64_t iv = (int64_t)v;
    if (scale) iv *= (int64_t)scale;
    return std::make_unique<IntLit>(p, iv);
}

bool Parser::tryParseTypeArgs(std::vector<TypeP>& out) {
    size_t save = i;
    quiet++;
    try {
        expect(T::LBracket, "'['");
        do {
            TypeP t = parseType();
            // Only builtin or Capitalized names: list[i] stays an index expression.
            std::function<bool(TypeExpr*)> ok = [&](TypeExpr* te) -> bool {
                if (te->k == TypeExpr::Named && !isBuiltinTypeName(te->name) && te->name != "nil" &&
                    !(std::isupper((unsigned char)te->name[0])))
                    return false;
                for (auto& a : te->args)
                    if (!ok(a.get())) return false;
                if (te->ret && !ok(te->ret.get())) return false;
                return true;
            };
            if (!ok(t.get())) throw ParseError{};
            out.push_back(std::move(t));
        } while (match(T::Comma));
        expect(T::RBracket, "']'");
    } catch (ParseError&) {
        quiet--;
        i = save;
        out.clear();
        return false;
    }
    quiet--;
    return true;
}

ExprP Parser::parsePrimary() {
    const Token& t = peek();
    SrcPos p{t.line, t.col};
    switch (t.kind) {
        case T::Int:
        case T::Float: advance(); return parseNumber(t);
        case T::Str: advance(); return std::make_unique<StrLit>(p, t.text);
        case T::FStr: advance(); return parseFString(t);
        case T::KwTrue: advance(); return std::make_unique<BoolLit>(p, true);
        case T::KwFalse: advance(); return std::make_unique<BoolLit>(p, false);
        case T::KwNil:
            advance();
            if (t.text == "null") deprecate(t, "'null' is 'nil'", {replaceTok(t, "nil")});
            return std::make_unique<NilLit>(p);
        case T::KwSelf: advance(); return std::make_unique<Ident>(p, "self");
        case T::Ident: {
            advance();
            const std::string& name = t.text;
            if ((name == "flow" || name == "list" || name == "map" || name == "set") && check(T::LBracket) &&
                !peek().spaceBefore) {
                std::vector<TypeP> targs;
                if (tryParseTypeArgs(targs)) {
                    auto call = std::make_unique<Call>(p, std::make_unique<Ident>(p, name));
                    call->typeArgs = std::move(targs);
                    if (check(T::LParen)) parseArgs(call->args, false);
                    return call;
                }
            }
            auto id = std::make_unique<Ident>(p, name);
            id->start = t.start;
            return id;
        }
        case T::LParen: {
            advance();
            int savedNoArrow = noArrow;
            noArrow = 0;
            if (match(T::RParen)) {
                noArrow = savedNoArrow;
                return std::make_unique<TupleLit>(p);
            }
            ExprP first = parseExpr();
            if (match(T::Comma)) {
                auto tup = std::make_unique<TupleLit>(p);
                tup->elems.push_back(std::move(first));
                while (!check(T::RParen)) {
                    tup->elems.push_back(parseExpr());
                    if (!match(T::Comma)) break;
                }
                expect(T::RParen, "')'");
                noArrow = savedNoArrow;
                return tup;
            }
            expect(T::RParen, "')'");
            noArrow = savedNoArrow;
            return first;
        }
        case T::LBracket: {
            advance();
            int savedNoArrow = noArrow;
            noArrow = 0;
            auto list = std::make_unique<ListLit>(p);
            while (!check(T::RBracket)) {
                list->elems.push_back(parseExpr());
                if (!match(T::Comma)) break;
            }
            expect(T::RBracket, "']'");
            noArrow = savedNoArrow;
            return list;
        }
        case T::LBrace: {
            advance();
            int savedNoArrow = noArrow;
            noArrow = 0;
            if (match(T::RBrace)) {
                noArrow = savedNoArrow;
                return std::make_unique<MapLit>(p);
            }
            ExprP first = parseExpr();
            if (match(T::Colon)) {
                auto m = std::make_unique<MapLit>(p);
                m->keys.push_back(std::move(first));
                m->values.push_back(parseExpr());
                while (match(T::Comma)) {
                    if (check(T::RBrace)) break;
                    m->keys.push_back(parseExpr());
                    expect(T::Colon, "':' between key and value");
                    m->values.push_back(parseExpr());
                }
                expect(T::RBrace, "'}'");
                noArrow = savedNoArrow;
                return m;
            }
            auto s = std::make_unique<SetLit>(p);
            s->elems.push_back(std::move(first));
            while (match(T::Comma)) {
                if (check(T::RBrace)) break;
                s->elems.push_back(parseExpr());
            }
            expect(T::RBrace, "'}'");
            noArrow = savedNoArrow;
            return s;
        }
        case T::KwFn: return parseLambda();
        case T::Arrow: return parseArrowLambda(false);
        case T::KwIf: advance(); return parseIf(p);
        case T::KwMatch: return parseMatch();
        case T::KwFork:
        case T::KwPar: {
            bool parallel = advance().kind == T::KwPar;
            noArrow++;
            ExprP target;
            try {
                target = parsePostfix(parsePrimary());
            } catch (...) {
                noArrow--;
                throw;
            }
            noArrow--;
            if (target->kind == NK::Ident) static_cast<Ident*>(target.get())->callee = true;
            if (check(T::Arrow)) {
                // fork fetch(url) -> page  <block>   runs the block in the task
                advance();
                const Token& n = expect(T::Ident, "a name for the task's result after '->'");
                SrcPos np{n.line, n.col};
                auto body = parseBlock();
                auto fn = std::make_unique<FnDecl>(p);
                fn->name = "<task>";
                fn->isLambda = true;
                fn->body = std::make_unique<Block>(p);
                fn->body->stmts.push_back(
                    std::make_unique<Assign>(np, std::make_unique<Ident>(np, n.text), std::move(target)));
                for (auto& s : body->stmts) fn->body->stmts.push_back(std::move(s));
                auto call = std::make_unique<Call>(p, std::make_unique<Lambda>(p, std::move(fn)));
                auto fk = std::make_unique<Fork>(p, std::move(call));
                fk->parallel = parallel;
                return fk;
            }
            auto fk = std::make_unique<Fork>(p, std::move(target));
            fk->parallel = parallel;
            return fk;
        }
        default: break;
    }
    failExpected("an expression");
}

ExprP Parser::parseLambda() {
    const Token& kw = expect(T::KwFn, "'fn'");
    SrcPos p{kw.line, kw.col};
    auto fn = std::make_unique<FnDecl>(p);
    fn->name = "<lambda>";
    fn->isLambda = true;
    if (match(T::LParen)) {
        fn->hasParamList = true;
        parseParams(fn->params, false);
        expect(T::RParen, "')'");
    }
    if (match(T::Colon)) fn->ret = parseType();  // fn(x): int = x * 2
    int savedArrow = arrowBody;
    arrowBody = 0;
    std::vector<bool> savedCalls;
    savedCalls.swap(callStack);
    struct Restore {
        std::vector<bool>& dst;
        std::vector<bool>& src;
        ~Restore() { dst.swap(src); }
    } restore{callStack, savedCalls};
    try {
        if (check(T::Arrow)) {
            const Token& arrow = peek();
            deprecate(arrow, "a lambda's body comes after '=' like a function's: fn(x) = x * 2",
                      {replaceTok(arrow, "=")});
        }
        if (match(T::Arrow) || match(T::Assign)) {
            SrcPos bp = pos();
            fn->body = std::make_unique<Block>(bp);
            int savedNoArrow = noArrow;
            noArrow = 0;
            fn->body->stmts.push_back(std::make_unique<ExprStmt>(bp, parseExpr()));
            noArrow = savedNoArrow;
        } else if (check(T::Newline) && peek(1).kind == T::Indent) {
            fn->body = parseBlock();
        } else {
            failExpected("'=' and an expression, or an indented body for the lambda");
        }
    } catch (...) {
        arrowBody = savedArrow;
        throw;
    }
    arrowBody = savedArrow;
    return std::make_unique<Lambda>(p, std::move(fn));
}

ExprP Parser::parseIf(SrcPos p) {
    auto node = std::make_unique<If>(p);
    noArrow++;
    try {
        node->cond = parseExpr();
    } catch (...) {
        noArrow--;
        throw;
    }
    noArrow--;
    node->then = parseBlock();
    bool indented = false;
    auto isElse = [](T k) { return k == T::KwElif || k == T::KwElse; };
    if (check(T::Newline) && isElse(peek(1).kind)) {
        advance();
    } else if (check(T::Newline) && peek(1).kind == T::Indent && isElse(peek(2).kind)) {
        // x = if a -> 1
        //     else -> 2
        advance();
        advance();
        indented = true;
    }
    if (check(T::KwElif)) {
        const Token& e = advance();
        SrcPos ep{e.line, e.col};
        auto blk = std::make_unique<Block>(ep);
        auto nested = parseIf(ep);
        static_cast<If*>(nested.get())->isElif = true;
        blk->stmts.push_back(std::make_unique<ExprStmt>(ep, std::move(nested)));
        node->otherwise = std::move(blk);
    } else if (match(T::KwElse)) {
        node->otherwise = parseElseBlock();
    }
    if (indented) {
        match(T::Newline);
        expect(T::Dedent, "end of the if expression");
    }
    return node;
}

// The block after `else`: an indented block, or a statement on the same line
// (`else "small"`; the old `else -> x` still works).
std::unique_ptr<Block> Parser::parseElseBlock() {
    if (check(T::Arrow)) {
        const Token& arrow = peek();
        deprecate(arrow, "'else' needs no arrow: else x", {{arrow.start, peek(1).start, ""}});
        return parseBlock();
    }
    if (check(T::Newline)) return parseBlock();
    SrcPos p = pos();
    auto block = std::make_unique<Block>(p);
    int savedNoArrow = noArrow;
    noArrow = 0;
    try {
        block->stmts.push_back(parseStatement());
    } catch (...) {
        noArrow = savedNoArrow;
        throw;
    }
    noArrow = savedNoArrow;
    return block;
}

// select
//     msg <: inbox -> handle(msg)
//     job <: jobs
//         run(job)
//     after 500 -> print("nothing for half a second")
//     else -> print("nothing ready")          (don't wait at all)
StmtP Parser::parseSelect() {
    const Token& kw = expect(T::KwSelect, "'select'");
    auto s = std::make_unique<Select>(SrcPos{kw.line, kw.col});
    expect(T::Newline, "end of line after 'select'");
    expect(T::Indent, "indented select arms (x <: flow, after ms, else)");
    bool sawAfter = false, sawElse = false;
    while (!check(T::Dedent) && !check(T::Eof)) {
        if (match(T::Newline)) continue;
        try {
            SelectArm arm;
            const Token& first = peek();
            arm.pos = {first.line, first.col};
            noArrow++;
            try {
                if (match(T::KwElse)) {
                    arm.kind = SelectArm::Else;
                    if (sawElse) fail(first, "a select has at most one 'else' arm");
                    sawElse = true;
                } else if (check(T::Ident) && first.text == "after" && peek(1).kind != T::Recv) {
                    advance();
                    arm.kind = SelectArm::After;
                    if (sawAfter) fail(first, "a select has at most one 'after' arm");
                    sawAfter = true;
                    arm.expr = parseExpr();
                } else {
                    const Token& n = expect(T::Ident, "a variable receiving from a flow (x <: flow), 'after ms' or 'else'");
                    arm.name = n.text;
                    arm.namePos = {n.line, n.col};
                    expect(T::Recv, "'<:' (x <: flow)");
                    arm.expr = parseExpr();
                }
            } catch (...) {
                noArrow--;
                throw;
            }
            noArrow--;
            arm.body = parseBlock();
            s->arms.push_back(std::move(arm));
            endStatement();
        } catch (ParseError&) {
            if (quiet > 0) throw;
            synchronize();
        }
    }
    expect(T::Dedent, "end of the select");
    if (sawAfter && sawElse) fail(kw, "a select can't have both 'after' (wait a while) and 'else' (don't wait)");
    bool anyRecv = false;
    for (auto& a : s->arms)
        if (a.kind == SelectArm::Recv) anyRecv = true;
    if (!anyRecv) fail(kw, "a select needs at least one 'x <: flow' arm");
    return s;
}

ExprP Parser::parseMatch() {
    const Token& kw = expect(T::KwMatch, "'match'");
    auto m = std::make_unique<Match>(SrcPos{kw.line, kw.col});
    noArrow++;
    try {
        m->subject = parseExpr();
    } catch (...) {
        noArrow--;
        throw;
    }
    noArrow--;
    expect(T::Newline, "end of line after the match subject");
    expect(T::Indent, "indented match arms");
    while (!check(T::Dedent) && !check(T::Eof)) {
        if (match(T::Newline)) continue;
        try {
            MatchArm arm;
            arm.pat = parsePattern();
            if (match(T::KwIf)) {
                noArrow++;
                try {
                    arm.guard = parseExpr();
                } catch (...) {
                    noArrow--;
                    throw;
                }
                noArrow--;
            }
            arm.body = parseBlock();
            m->arms.push_back(std::move(arm));
            endStatement();
        } catch (ParseError&) {
            if (quiet > 0) throw;
            synchronize();
        }
    }
    expect(T::Dedent, "end of match");
    return m;
}

ExprP Parser::parseFString(const Token& t) {
    SrcPos p{t.line, t.col};
    auto fs = std::make_unique<FStr>(p);
    const std::string& raw = t.text;
    int prefix = (t.end - t.start == raw.size() + 7) ? 4 : 2;
    std::string text;
    auto flushText = [&]() {
        if (text.empty()) return;
        std::string val, msg;
        if (!unescapeString(text, val, msg) && quiet == 0) diags.error(file, p, msg);
        FStr::Part part;
        part.text = std::move(val);
        fs->parts.push_back(std::move(part));
        text.clear();
    };
    size_t k = 0;
    while (k < raw.size()) {
        char c = raw[k];
        if (c == '\\' && k + 1 < raw.size()) {
            text += c;
            text += raw[k + 1];
            k += 2;
            continue;
        }
        if (c == '{' && k + 1 < raw.size() && raw[k + 1] == '{') {
            text += "\\{";
            k += 2;
            continue;
        }
        if (c == '}' && k + 1 < raw.size() && raw[k + 1] == '}') {
            text += "\\}";
            k += 2;
            continue;
        }
        if (c != '{') {
            text += c;
            k++;
            continue;
        }
        flushText();
        // find the matching '}' and an optional top-level ':' format spec
        size_t start = k + 1, j = start;
        int depth = 0;
        size_t colon = std::string::npos;
        while (j < raw.size()) {
            char d = raw[j];
            if (d == '"' || d == '\'') {
                char q = d;
                j++;
                while (j < raw.size() && raw[j] != q) {
                    if (raw[j] == '\\') j++;
                    j++;
                }
            } else if (d == '(' || d == '[' || d == '{') {
                depth++;
            } else if (d == ')' || d == ']') {
                depth--;
            } else if (d == '}') {
                if (depth == 0) break;
                depth--;
            } else if (d == ':' && depth == 0 && colon == std::string::npos) {
                colon = j;
            }
            j++;
        }
        if (j >= raw.size()) {
            if (quiet == 0) diags.error(file, p, "unclosed '{' in f-string");
            break;
        }
        std::string exprSrc = raw.substr(start, (colon == std::string::npos ? j : colon) - start);
        FStr::Part part;
        if (colon != std::string::npos) part.spec = raw.substr(colon + 1, j - colon - 1);
        Lexer lx(exprSrc, file, diags);
        auto subToks = lx.tokenize();
        for (auto& st : subToks) {
            st.line += t.line - 1;
            if (st.line == t.line) st.col += t.col + prefix + (int)start - 1;
            st.start += t.start + prefix + start;  // offsets in the file (for fixes)
            st.end += t.start + prefix + start;
        }
        Parser sub(std::move(subToks), file, diags, opts);
        sub.quiet = quiet;
        part.expr = sub.parseStandaloneExpr();
        if (!part.expr) part.expr = std::make_unique<NilLit>(p);
        fs->parts.push_back(std::move(part));
        k = j + 1;
    }
    flushText();
    return fs;
}

// ------------------------------------------------------------------ patterns

PatternP Parser::parsePattern() {
    PatternP p = parseAltPattern();
    if (check(T::KwAs)) {
        advance();
        const Token& n = expect(T::Ident, "a name after 'as'");
        auto as = std::make_unique<Pattern>(Pattern::As, SrcPos{n.line, n.col});
        as->name = n.text;
        as->subs.push_back(std::move(p));
        return as;
    }
    return p;
}

PatternP Parser::parseAltPattern() {
    PatternP first = parsePrimaryPattern();
    if (!check(T::Pipe)) return first;
    auto alt = std::make_unique<Pattern>(Pattern::Alt, first->pos);
    alt->subs.push_back(std::move(first));
    while (match(T::Pipe)) alt->subs.push_back(parsePrimaryPattern());
    return alt;
}

PatternP Parser::parsePrimaryPattern() {
    const Token& t = peek();
    SrcPos p{t.line, t.col};
    auto literalOrRange = [&](ExprP lit) -> PatternP {
        if (check(T::DotDot) || check(T::DotDotLess) || check(T::DotDotEq)) {
            const Token& op = advance();
            if (op.kind == T::DotDotLess) deprecate(op, "'..<' is '..' (the end isn't included)", {replaceTok(op, "..")});
            bool inclusive = op.kind == T::DotDotEq;
            auto r = std::make_unique<Pattern>(Pattern::RangeP, p);
            r->value = std::move(lit);
            r->inclusive = inclusive;
            const Token& hi = peek();
            SrcPos hp{hi.line, hi.col};
            if (match(T::Minus)) {
                const Token& num = advance();
                if (num.kind != T::Int && num.kind != T::Float) fail(num, "expected a number");
                ExprP v = parseNumber(num);
                r->value2 = std::make_unique<Unary>(hp, UnOp::Neg, std::move(v));
            } else if (check(T::Int) || check(T::Float)) {
                r->value2 = parseNumber(advance());
            } else if (check(T::Str)) {
                r->value2 = std::make_unique<StrLit>(hp, advance().text);
            } else {
                failExpected("the upper bound of the range pattern");
            }
            return r;
        }
        auto l = std::make_unique<Pattern>(Pattern::Literal, p);
        l->value = std::move(lit);
        return l;
    };
    switch (t.kind) {
        case T::Int:
        case T::Float: advance(); return literalOrRange(parseNumber(t));
        case T::Minus: {
            advance();
            const Token& num = advance();
            if (num.kind != T::Int && num.kind != T::Float) fail(num, "expected a number after '-'");
            return literalOrRange(std::make_unique<Unary>(p, UnOp::Neg, parseNumber(num)));
        }
        case T::Str: advance(); return literalOrRange(std::make_unique<StrLit>(p, t.text));
        case T::KwTrue: advance(); return literalOrRange(std::make_unique<BoolLit>(p, true));
        case T::KwFalse: advance(); return literalOrRange(std::make_unique<BoolLit>(p, false));
        case T::KwNil:
            advance();
            if (t.text == "null") deprecate(t, "'null' is 'nil'", {replaceTok(t, "nil")});
            return literalOrRange(std::make_unique<NilLit>(p));
        case T::LParen: {
            advance();
            auto tup = std::make_unique<Pattern>(Pattern::TupleP, p);
            bool sawComma = false;
            while (!check(T::RParen)) {
                tup->subs.push_back(parsePattern());
                if (!match(T::Comma)) break;
                sawComma = true;
            }
            expect(T::RParen, "')'");
            if (tup->subs.size() == 1 && !sawComma) return std::move(tup->subs[0]);
            return tup;
        }
        case T::LBrace: {
            // {"name": n, "age": 30}: a map with (at least) these keys
            advance();
            auto mp = std::make_unique<Pattern>(Pattern::MapP, p);
            while (!check(T::RBrace)) {
                const Token& k = peek();
                SrcPos kp{k.line, k.col};
                ExprP key;
                if (match(T::Str)) {
                    key = std::make_unique<StrLit>(kp, k.text);
                } else if (check(T::Int) || check(T::Float)) {
                    key = parseNumber(advance());
                } else if (match(T::Minus)) {
                    const Token& num = advance();
                    if (num.kind != T::Int && num.kind != T::Float) fail(num, "expected a number after '-'");
                    key = std::make_unique<Unary>(kp, UnOp::Neg, parseNumber(num));
                } else if (match(T::KwTrue)) {
                    key = std::make_unique<BoolLit>(kp, true);
                } else if (match(T::KwFalse)) {
                    key = std::make_unique<BoolLit>(kp, false);
                } else {
                    failExpected("a key (a string or number) in the map pattern");
                }
                expect(T::Colon, "':' after the key");
                mp->keys.push_back(std::move(key));
                mp->subs.push_back(parsePattern());
                if (!match(T::Comma)) break;
            }
            expect(T::RBrace, "'}'");
            return mp;
        }
        case T::LBracket: {
            advance();
            auto lst = std::make_unique<Pattern>(Pattern::ListP, p);
            while (!check(T::RBracket)) {
                if (check(T::DotDot)) {
                    const Token& dd = advance();
                    if (lst->restIndex >= 0) fail(dd, "only one '..rest' allowed in a list pattern");
                    lst->restIndex = (int)lst->subs.size();
                    lst->restPos = {dd.line, dd.col};
                    if (check(T::Ident)) {
                        const Token& rn = advance();
                        lst->restName = rn.text == "_" ? "" : rn.text;
                        lst->restPos = {rn.line, rn.col};
                    }
                } else {
                    lst->subs.push_back(parsePattern());
                }
                if (!match(T::Comma)) break;
            }
            expect(T::RBracket, "']'");
            return lst;
        }
        case T::Ident: {
            advance();
            const std::string& name = t.text;
            if (name == "_") return std::make_unique<Pattern>(Pattern::Wildcard, p);
            if (check(T::Dot)) {
                // Error.NotFound, Color.RED : compare against the value
                ExprP e = std::make_unique<Ident>(p, name);
                std::string dotted = name;
                while (match(T::Dot)) {
                    const Token& mn = expect(T::Ident, "a member name");
                    dotted += "." + mn.text;
                    e = std::make_unique<Member>(p, std::move(e), mn.text, SrcPos{mn.line, mn.col});
                }
                if (check(T::LParen)) {
                    // enum variant with fields: Shape.Circle(r)
                    advance();
                    auto cp = std::make_unique<Pattern>(Pattern::ClassP, p);
                    cp->name = dotted;
                    cp->value = std::move(e);
                    cp->inclusive = true;
                    while (!check(T::RParen)) {
                        cp->subs.push_back(parsePattern());
                        if (!match(T::Comma)) break;
                    }
                    expect(T::RParen, "')'");
                    return cp;
                }
                auto v = std::make_unique<Pattern>(Pattern::Value, p);
                v->value = std::move(e);
                return v;
            }
            if (isBuiltinTypeName(name) || name == "Error") {
                auto tp = std::make_unique<Pattern>(Pattern::TypeP_, p);
                tp->name = name;
                return tp;
            }
            if (std::isupper((unsigned char)name[0])) {
                auto cp = std::make_unique<Pattern>(Pattern::ClassP, p);
                cp->name = name;
                cp->value = std::make_unique<Ident>(p, name);
                if (match(T::LParen)) {
                    cp->inclusive = true;  // has a field list
                    while (!check(T::RParen)) {
                        cp->subs.push_back(parsePattern());
                        if (!match(T::Comma)) break;
                    }
                    expect(T::RParen, "')'");
                }
                return cp;
            }
            auto b = std::make_unique<Pattern>(Pattern::Bind, p);
            b->name = name;
            return b;
        }
        default: break;
    }
    failExpected("a pattern");
}

// ------------------------------------------------------------------ types

TypeP Parser::parseTypeText(const std::string& text, bool* variadic) {
    std::string t = text;
    bool va = false;
    size_t dots = t.find("...");
    if (dots != std::string::npos) {
        va = true;
        t.erase(dots, 3);
    }
    if (variadic) *variadic = va;
    Diagnostics d;
    Lexer lx(t, "<sig>", d);
    Parser p(lx.tokenize(), "<sig>", d);
    p.quiet = 1;
    try {
        TypeP ty = p.parseType();
        if (p.check(T::Bang)) {
            p.advance();
            auto res = std::make_unique<TypeExpr>();
            res->k = TypeExpr::Result;
            res->pos = ty->pos;
            if (ty->k == TypeExpr::Fn && ty->ret) {
                // "fn(str) -> str ! Error": the error applies to the return type
                auto inner = std::make_unique<TypeExpr>();
                inner->k = TypeExpr::Result;
                inner->args.push_back(std::move(ty->ret));
                inner->ret = p.parseType();
                ty->ret = std::move(inner);
                return ty;
            }
            res->args.push_back(std::move(ty));
            res->ret = p.parseType();
            return res;
        }
        return ty;
    } catch (ParseError&) {
        return nullptr;
    }
}

TypeP Parser::parseType() {
    TypeP t = parseTypeAtom();
    while (check(T::Question)) {
        advance();
        auto opt = std::make_unique<TypeExpr>();
        opt->k = TypeExpr::Optional;
        opt->pos = t->pos;
        opt->args.push_back(std::move(t));
        t = std::move(opt);
    }
    if (check(T::Bang)) {
        // T ! E: a value or an error; T! is T ! Error
        advance();
        auto res = std::make_unique<TypeExpr>();
        res->k = TypeExpr::Result;
        res->pos = t->pos;
        res->args.push_back(std::move(t));
        T k = peek().kind;
        if (k == T::Ident || k == T::LParen || k == T::KwFn || k == T::KwNil || k == T::Amp) {
            res->ret = parseType();
        } else {
            auto err = std::make_unique<TypeExpr>();
            err->name = "Error";
            err->pos = res->pos;
            res->ret = std::move(err);
        }
        return res;
    }
    return t;
}

TypeP Parser::parseTypeAtom() {
    const Token& t = peek();
    auto te = std::make_unique<TypeExpr>();
    te->pos = {t.line, t.col};
    if (match(T::Amp)) {
        te->k = TypeExpr::Ref;
        if (check(T::Ident) && peek().text == "mut") {
            advance();
            te->mut = true;
        }
        te->args.push_back(parseType());
        return te;
    }
    if (match(T::LParen)) {
        te->k = TypeExpr::Tuple;
        while (!check(T::RParen)) {
            te->args.push_back(parseType());
            if (!match(T::Comma)) break;
        }
        expect(T::RParen, "')'");
        return te;
    }
    if (match(T::KwFn)) {
        te->k = TypeExpr::Fn;
        if (match(T::LParen)) {
            while (!check(T::RParen)) {
                te->args.push_back(parseType());
                if (!match(T::Comma)) break;
            }
            expect(T::RParen, "')'");
        }
        if (match(T::Arrow)) te->ret = parseType();
        return te;
    }
    if (check(T::KwNil)) {
        const Token& n = advance();
        if (n.text == "null") deprecate(n, "'null' is 'nil'", {replaceTok(n, "nil")});
        te->name = "nil";
        return te;
    }
    const Token& name = expect(T::Ident, "a type");
    te->name = name.text;
    te->typeVar = isTypeParam(name.text);
    std::vector<TextEdit> resultFix;
    if (check(T::LBracket)) {
        const Token& lb = advance();
        resultFix.push_back({name.start, lb.end, ""});
        while (!check(T::RBracket)) {
            te->args.push_back(parseType());
            if (!check(T::Comma)) break;
            const Token& comma = advance();
            if (te->args.size() == 1) resultFix.push_back({comma.start, peek().start, " ! "});
        }
        const Token& rb = expect(T::RBracket, "']'");
        resultFix.push_back({rb.start, rb.end, ""});
    }
    if (te->name == "Result") {
        if (te->args.size() == 2) deprecate(name, "Result[T, E] is written T ! E", std::move(resultFix));
        // Result[T, E]  ==  T ! E
        auto res = std::make_unique<TypeExpr>();
        res->k = TypeExpr::Result;
        res->pos = te->pos;
        if (te->args.empty()) {
            auto any = std::make_unique<TypeExpr>();
            any->name = "any";
            res->args.push_back(std::move(any));
        } else {
            res->args.push_back(std::move(te->args[0]));
        }
        if (te->args.size() > 1) res->ret = std::move(te->args[1]);
        return res;
    }
    return te;
}

}  // namespace ixo
