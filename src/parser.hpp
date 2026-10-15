#pragma once

#include <functional>

#include "ast.hpp"
#include "lexer.hpp"

namespace ixo {

std::string cleanDocstring(const std::string& raw);

struct ParseOptions {
    bool prelude = false;  // allow `self` as a first parameter (extension methods)
};

class Parser {
public:
    Parser(std::vector<Token> tokens, std::string file, Diagnostics& diags, ParseOptions opts = {});
    std::unique_ptr<Block> parseProgram();
    bool strict = false;  // $strict at the top of the file
    // Parses a single expression (f-string interpolation, REPL helpers).
    ExprP parseStandaloneExpr();
    // Parses a type written as text, e.g. "fn(str) -> int ! Error" (builtin
    // signatures). `...` after the last parameter marks a variadic function.
    static TypeP parseTypeText(const std::string& text, bool* variadic);

private:
    struct ParseError {};

    std::vector<Token> toks;
    size_t i = 0;
    std::string file;
    Diagnostics& diags;
    ParseOptions opts;
    int noArrow = 0;    // >0: a '->' after an expression is not a trailing lambda
    int sliceContext = 0;  // >0: inside xs[...]: ranges may leave out an end
    int arrowBody = 0;  // >0: inside an arrow lambda body; placeholders belong to it
    int quiet = 0;      // >0: speculative parse, errors are not reported
    // Calls whose arguments are being parsed (true = method call x.f(...)).
    // Placeholder lambdas form at the nearest method-call argument.
    std::vector<bool> callStack;
    // Type parameters of the classes / functions being parsed (Key in class Cache[Key, Value])
    std::vector<std::vector<std::string>> typeParamScopes;
    bool isTypeParam(const std::string& n) const {
        if (n.size() == 1 && std::isupper((unsigned char)n[0])) return true;
        for (auto& sc : typeParamScopes)
            for (auto& tp : sc)
                if (tp == n) return true;
        return false;
    }

    const Token& peek(size_t k = 0) const { return toks[std::min(i + k, toks.size() - 1)]; }
    const Token& prev() const { return toks[i > 0 ? i - 1 : 0]; }
    bool check(T k) const { return peek().kind == k; }
    // True right after an indented block closed: the expression is complete,
    // so the next line is a new statement (not a call or an operator).
    bool blockEnded() const { return i > 0 && toks[i - 1].kind == T::Dedent; }
    bool match(T k) {
        if (!check(k)) return false;
        i++;
        return true;
    }
    const Token& advance() {
        const Token& t = toks[i];
        if (i < toks.size() - 1) i++;
        return t;
    }
    const Token& expect(T k, const char* what);
    [[noreturn]] void fail(const Token& at, const std::string& msg);
    [[noreturn]] void failExpected(const char* what);
    SrcPos pos() const { return {peek().line, peek().col}; }
    void synchronize();
    void endStatement();
    // An old spelling that still works: a warning whose fix writes the new one.
    void deprecate(const Token& at, const std::string& msg, std::vector<TextEdit> fix);
    // ' and ' for `&&` (spaces only where the source has none)
    std::string spaced(const Token& t, const std::string& word) const;
    std::unique_ptr<Block> parseElseBlock();

    // statements
    StmtP parseStatement();
    StmtP parseSimpleStatement();
    std::unique_ptr<Block> parseBlock(bool allowEmpty = false);
    std::unique_ptr<Block> parseStatementsUntilDedent(SrcPos p);
    std::unique_ptr<FnDecl> parseFnDecl(std::vector<std::string> attrs, bool isFork, bool inClass, bool isPar = false);
    StmtP parseClass(bool isData);
    void addDataMethods(ClassDecl* c);
    void addUnpacks(FnDecl* fn);
    ExprP cloneSimple(Expr* e);
    std::unique_ptr<FnDecl> parseFnDeclInner(std::vector<std::string> attrs, bool isFork, bool inClass, bool isPar);
    std::vector<StmtP> pending;  // statements that follow the one just parsed (use x = ...: its defer)
    int unpackCount = 0;
    StmtP parseInterface();
    StmtP parseEnum();
    StmtP parseUse();
    StmtP parseUseSelective();
    StmtP parseFor();
    StmtP parseAttributed();
    void parseParams(std::vector<Param>& params, bool allowSelf);

    // expressions
    ExprP parseExpr();
    ExprP parseSend();
    ExprP parseCatch();
    ExprP parseOr();
    ExprP parseAnd();
    ExprP parseNot();
    ExprP parseComparison();
    ExprP parseRange();
    ExprP parseBitOr();
    ExprP parseBitXor();
    ExprP parseBitAnd();
    ExprP parseShift();
    ExprP parseAdd();
    ExprP parseMul();
    ExprP parseUnary();
    ExprP parsePower(ExprP base);
    ExprP parsePostfix(ExprP e);
    ExprP parsePrimary();
    ExprP parseIf(SrcPos p);
    ExprP parseMatch();
    StmtP parseSelect();
    ExprP parseFString(const Token& t);
    ExprP parseLambda();
    ExprP parseArrowLambda(bool statementBody);
    ExprP parseNumber(const Token& t);
    void parseArgs(std::vector<ExprP>& args, bool methodCall);
    ExprP parseArg();
    ExprP wrapPlaceholders(ExprP e);
    bool tryParseTypeArgs(std::vector<TypeP>& out);

    // patterns & types
    PatternP parsePattern();
    PatternP parseAltPattern();
    PatternP parsePrimaryPattern();
    TypeP parseType();
    TypeP parseTypeAtom();
};

}  // namespace ixo
