// Lexer: turns Ixo source into tokens, including the INDENT / DEDENT /
// NEWLINE layout tokens that make indentation significant.
#pragma once

#include "common.hpp"

namespace ixo {

enum class T : uint8_t {
    Int, Float, Str, FStr, Ident,
    // keywords
    KwFn, KwClass, KwIf, KwElif, KwElse, KwWhile, KwFor, KwIn, KwNot, KwAnd, KwOr, KwIs,
    KwReturn, KwMatch, KwUse, KwVar, KwWith, KwFork, KwTrue, KwFalse, KwNil, KwBreak,
    KwContinue, KwRetry, KwSelf, KwAs, KwPar, KwDefer, KwInterface, KwEnum, KwSelect, KwTodo,
    // layout
    Newline, Indent, Dedent, Eof,
    // operators
    Plus, Minus, Star, Slash, SlashSlash, TildeSlash, Percent, StarStar, PlusPlus, MinusMinus,
    Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign, StarStarAssign,
    EqEq, NotEq, Lt, Gt, LtEq, GtEq, EqEqEq, NotEqEq,
    Amp, Pipe, Caret, Tilde, Shl, Shr, AmpAmp, PipePipe,
    Arrow, Send, Recv,
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Comma, Colon, Dot, DotDot, DotDotLess, DotDotEq, Question, Bang, At, Dollar,
    // trivia (only produced when keepTrivia is on, for the formatter)
    Comment,
    Error,
};

const char* tokenName(T t);

struct Token {
    T kind = T::Error;
    std::string text;     // identifier name, processed string value, or number text
    int line = 1, col = 1;
    size_t start = 0, end = 0;  // byte offsets of the raw lexeme in the source
    bool spaceBefore = false;   // whitespace directly before this token
    bool newlineBefore = false; // token starts a new physical line
    int blankLinesBefore = 0;   // empty lines directly before (formatter)
};

struct LexOptions {
    bool keepTrivia = false;  // emit Comment tokens (formatter)
};

class Lexer {
public:
    Lexer(const std::string& source, std::string file, Diagnostics& diags, LexOptions opts = {});
    std::vector<Token> tokenize();

private:
    const std::string& src;
    std::string file;
    Diagnostics& diags;
    LexOptions opts;
    size_t pos = 0;
    int line = 1, col = 1;
    std::vector<int> indents{0};
    int bracketDepth = 0;
    // Layout islands: inside brackets, a block (a lambda body, match arms, an
    // if/else block) re-enables indentation until the block ends.
    struct Island {
        int depth;          // bracket depth the island lives at
        size_t indentSize;  // size of `indents` before the island's base was pushed
    };
    std::vector<Island> islands;
    std::vector<bool> parenAfterFn;   // per open bracket: a '(' right after `fn`
    bool lastCloseFnParams = false;   // the last token is the ')' of `fn(...)`
    int lineIndent = 0;               // indentation of the current physical line
    size_t lineFirstTok = 0;          // first token of the current physical line
    int lineStartDepth = 0;           // bracket depth where that line started
    bool layoutActive() const { return islands.empty() ? bracketDepth == 0 : bracketDepth == islands.back().depth; }
    bool blockHeaderLine() const;
    int nextLineIndent() const;       // indentation of the next code line (-1 at the end)
    void closeIsland();
    void pushLayout(T kind);
    std::vector<Token> out;
    bool spaceBefore = false;
    bool newlineBefore = true;
    int blankLines = 0;

    char cur() const { return pos < src.size() ? src[pos] : '\0'; }
    char at(size_t off) const { return pos + off < src.size() ? src[pos + off] : '\0'; }
    void adv();
    void push(T kind, std::string text, size_t start, int line, int col);
    bool handleLineStart();
    void lexNumber();
    void lexIdent();
    void lexString(char quote, bool fstring, bool rawString = false);
    void lexOperator();
    void error(const std::string& msg) { diags.error(file, {line, col}, msg); }
};

// Processes escape sequences of a string literal body. Returns false on a
// malformed escape (msg describes it).
bool unescapeString(std::string_view raw, std::string& outStr, std::string& msg);

}  // namespace ixo
