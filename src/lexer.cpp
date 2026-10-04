#include "lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace ixo {

static const std::unordered_map<std::string, T>& keywords() {
    static const std::unordered_map<std::string, T> kw = {
        {"fn", T::KwFn},         {"class", T::KwClass},   {"if", T::KwIf},
        {"elif", T::KwElif},     {"else", T::KwElse},     {"while", T::KwWhile},
        {"for", T::KwFor},       {"in", T::KwIn},         {"not", T::KwNot},
        {"and", T::KwAnd},       {"or", T::KwOr},         {"is", T::KwIs},
        {"return", T::KwReturn}, {"match", T::KwMatch},   {"use", T::KwUse},
        {"var", T::KwVar},       {"with", T::KwWith},     {"fork", T::KwFork},
        {"true", T::KwTrue},     {"false", T::KwFalse},   {"nil", T::KwNil},
        {"null", T::KwNil},      {"break", T::KwBreak},   {"continue", T::KwContinue},
        {"retry", T::KwRetry},   {"self", T::KwSelf},     {"as", T::KwAs},
        {"par", T::KwPar},       {"defer", T::KwDefer},   {"todo", T::KwTodo},
        {"interface", T::KwInterface}, {"enum", T::KwEnum},  {"select", T::KwSelect},
    };
    return kw;
}

const char* tokenName(T t) {
    switch (t) {
        case T::Int: return "integer";
        case T::Float: return "float";
        case T::Str: return "string";
        case T::FStr: return "f-string";
        case T::Ident: return "identifier";
        case T::KwFn: return "'fn'";
        case T::KwClass: return "'class'";
        case T::KwIf: return "'if'";
        case T::KwElif: return "'elif'";
        case T::KwElse: return "'else'";
        case T::KwWhile: return "'while'";
        case T::KwFor: return "'for'";
        case T::KwIn: return "'in'";
        case T::KwNot: return "'not'";
        case T::KwAnd: return "'and'";
        case T::KwOr: return "'or'";
        case T::KwIs: return "'is'";
        case T::KwReturn: return "'return'";
        case T::KwMatch: return "'match'";
        case T::KwUse: return "'use'";
        case T::KwVar: return "'var'";
        case T::KwWith: return "'with'";
        case T::KwFork: return "'fork'";
        case T::KwTrue: return "'true'";
        case T::KwFalse: return "'false'";
        case T::KwNil: return "'nil'";
        case T::KwBreak: return "'break'";
        case T::KwContinue: return "'continue'";
        case T::KwRetry: return "'retry'";
        case T::KwSelf: return "'self'";
        case T::KwAs: return "'as'";
        case T::KwPar: return "'par'";
        case T::KwDefer: return "'defer'";
        case T::KwInterface: return "'interface'";
        case T::KwEnum: return "'enum'";
        case T::KwSelect: return "'select'";
        case T::KwTodo: return "'todo'";
        case T::Newline: return "end of line";
        case T::Indent: return "indentation";
        case T::Dedent: return "end of block";
        case T::Eof: return "end of file";
        case T::Plus: return "'+'";
        case T::Minus: return "'-'";
        case T::Star: return "'*'";
        case T::Slash: return "'/'";
        case T::SlashSlash: return "'//'";
        case T::Percent: return "'%'";
        case T::StarStar: return "'**'";
        case T::PlusPlus: return "'++'";
        case T::MinusMinus: return "'--'";
        case T::Assign: return "'='";
        case T::PlusAssign: return "'+='";
        case T::MinusAssign: return "'-='";
        case T::StarAssign: return "'*='";
        case T::SlashAssign: return "'/='";
        case T::PercentAssign: return "'%='";
        case T::StarStarAssign: return "'**='";
        case T::EqEq: return "'=='";
        case T::NotEq: return "'!='";
        case T::Lt: return "'<'";
        case T::Gt: return "'>'";
        case T::LtEq: return "'<='";
        case T::GtEq: return "'>='";
        case T::EqEqEq: return "'==='";
        case T::NotEqEq: return "'!=='";
        case T::Amp: return "'&'";
        case T::Pipe: return "'|'";
        case T::Caret: return "'^'";
        case T::Tilde: return "'~'";
        case T::TildeSlash: return "'~/'";
        case T::Shl: return "'<<'";
        case T::Shr: return "'>>'";
        case T::AmpAmp: return "'&&'";
        case T::PipePipe: return "'||'";
        case T::Arrow: return "'->'";
        case T::Send: return "':>'";
        case T::Recv: return "'<:'";
        case T::LParen: return "'('";
        case T::RParen: return "')'";
        case T::LBracket: return "'['";
        case T::RBracket: return "']'";
        case T::LBrace: return "'{'";
        case T::RBrace: return "'}'";
        case T::Comma: return "','";
        case T::Colon: return "':'";
        case T::Dot: return "'.'";
        case T::DotDot: return "'..'";
        case T::DotDotLess: return "'..<'";
        case T::DotDotEq: return "'..='";
        case T::Question: return "'?'";
        case T::Bang: return "'!'";
        case T::At: return "'@'";
        case T::Dollar: return "'$'";
        case T::Comment: return "comment";
        case T::Error: return "invalid token";
    }
    return "?";
}

Lexer::Lexer(const std::string& source, std::string file, Diagnostics& diags, LexOptions opts)
    : src(source), file(std::move(file)), diags(diags), opts(opts) {
    // Skip a UTF-8 byte order mark.
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF)
        pos = 3;
}

void Lexer::adv() {
    if (pos >= src.size()) return;
    if (src[pos] == '\n') {
        line++;
        col = 1;
    } else {
        col++;
    }
    pos++;
}

void Lexer::push(T kind, std::string text, size_t start, int l, int c) {
    lastCloseFnParams = false;
    Token t;
    t.kind = kind;
    t.text = std::move(text);
    t.line = l;
    t.col = c;
    t.start = start;
    t.end = pos;
    t.spaceBefore = spaceBefore;
    t.newlineBefore = newlineBefore;
    t.blankLinesBefore = blankLines;
    out.push_back(std::move(t));
    spaceBefore = false;
    newlineBefore = false;
    blankLines = 0;
}

static bool isLayout(T k) { return k == T::Newline || k == T::Indent || k == T::Dedent; }

void Lexer::pushLayout(T kind) {
    Token t;
    t.kind = kind;
    t.line = line;
    t.col = col;
    t.start = t.end = pos;
    out.push_back(t);
}

void Lexer::closeIsland() {
    Island is = islands.back();
    bool needNewline = false;
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
        if (it->kind == T::Comment) continue;
        needNewline = !isLayout(it->kind);
        break;
    }
    if (needNewline) pushLayout(T::Newline);
    while (indents.size() > is.indentSize + 1) {
        indents.pop_back();
        pushLayout(T::Dedent);
    }
    indents.resize(is.indentSize);
    islands.pop_back();
}

// Does the current physical line (inside brackets) end with a block header:
// `fn(x)`, `fn`, `else`, or `match x` / `if cond` / `elif cond` without `->`?
bool Lexer::blockHeaderLine() const {
    size_t last = out.size();
    while (last > lineFirstTok && out[last - 1].kind == T::Comment) last--;
    if (last <= lineFirstTok) return false;
    T k = out[last - 1].kind;
    if (k == T::KwFn || k == T::KwElse) return true;
    if (k == T::RParen && lastCloseFnParams) return true;
    int d = lineStartDepth;
    long cand = -1;
    for (size_t i = lineFirstTok; i < last; i++) {
        T t = out[i].kind;
        if (d == bracketDepth) {
            if (t == T::KwMatch || t == T::KwIf || t == T::KwElif) cand = (long)i;
            if (t == T::Arrow) cand = -1;
        }
        if (t == T::LParen || t == T::LBracket || t == T::LBrace) d++;
        if (t == T::RParen || t == T::RBracket || t == T::RBrace) d--;
    }
    return cand >= 0;
}

int Lexer::nextLineIndent() const {
    size_t p = pos;
    while (p < src.size()) {
        int w = 0;
        while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) {
            w += src[p] == '\t' ? 4 : 1;
            p++;
        }
        if (p >= src.size()) return -1;
        if (src[p] == '\n' || src[p] == '\r' || src[p] == '#') {
            while (p < src.size() && src[p] != '\n') p++;
            p++;
            continue;
        }
        return w;
    }
    return -1;
}

// Called at the start of each physical line outside brackets. Skips blank and
// comment-only lines, then emits INDENT/DEDENT tokens. Returns false at EOF.
bool Lexer::handleLineStart() {
    while (true) {
        int width = 0;
        while (cur() == ' ' || cur() == '\t') {
            width += cur() == '\t' ? 4 : 1;
            adv();
        }
        char c = cur();
        if (c == '\0') return false;
        if (c == '\n' || c == '\r') {
            if (c == '\r' && at(1) == '\n') adv();
            adv();
            blankLines++;
            newlineBefore = true;
            continue;
        }
        if (c == '#') {
            size_t start = pos;
            int l = line, cl = col;
            while (cur() != '\0' && cur() != '\n' && cur() != '\r') adv();
            if (opts.keepTrivia) {
                newlineBefore = true;
                push(T::Comment, src.substr(start, pos - start), start, l, cl);
            }
            if (cur() == '\r' && at(1) == '\n') adv();
            adv();
            newlineBefore = true;
            continue;
        }
        newlineBefore = true;
        lineIndent = width;
        if (!islands.empty() && bracketDepth == islands.back().depth && width <= indents[islands.back().indentSize]) {
            // back at (or left of) the line that opened the block: the island ends
            closeIsland();
            lineFirstTok = out.size();
            lineStartDepth = bracketDepth;
            return true;
        }
        int top = indents.back();
        if (width > top) {
            indents.push_back(width);
            push(T::Indent, "", pos, line, col);
            newlineBefore = true;
        } else if (width < top) {
            while (indents.back() > width) {
                indents.pop_back();
                push(T::Dedent, "", pos, line, col);
                newlineBefore = true;
            }
            if (indents.back() != width) {
                error("this line's indentation doesn't match any enclosing block");
                indents.push_back(width);
            }
        }
        return true;
    }
}

void Lexer::lexNumber() {
    size_t start = pos;
    int l = line, c = col;
    std::string text;
    bool isFloat = false;
    if (cur() == '0' && (at(1) == 'x' || at(1) == 'X' || at(1) == 'b' || at(1) == 'B')) {
        bool hex = at(1) == 'x' || at(1) == 'X';
        text += '0';
        text += hex ? 'x' : 'b';
        adv();
        adv();
        while (hex ? std::isxdigit((unsigned char)cur()) || cur() == '_' : cur() == '0' || cur() == '1' || cur() == '_') {
            if (cur() != '_') text += cur();
            adv();
        }
        if (text.size() == 2) error("expected digits after number prefix");
    } else {
        while (std::isdigit((unsigned char)cur()) || cur() == '_') {
            if (cur() != '_') text += cur();
            adv();
        }
        if (cur() == '.' && std::isdigit((unsigned char)at(1))) {
            isFloat = true;
            text += '.';
            adv();
            while (std::isdigit((unsigned char)cur()) || cur() == '_') {
                if (cur() != '_') text += cur();
                adv();
            }
        }
        if ((cur() == 'e' || cur() == 'E') &&
            (std::isdigit((unsigned char)at(1)) ||
             ((at(1) == '+' || at(1) == '-') && std::isdigit((unsigned char)at(2))))) {
            isFloat = true;
            text += 'e';
            adv();
            if (cur() == '+' || cur() == '-') {
                text += cur();
                adv();
            }
            while (std::isdigit((unsigned char)cur())) {
                text += cur();
                adv();
            }
        }
    }
    // Duration suffixes: 500ms, 2s  (value in milliseconds)
    if (std::isalpha((unsigned char)cur())) {
        std::string suffix;
        while (std::isalnum((unsigned char)cur()) || cur() == '_') {
            suffix += cur();
            adv();
        }
        if (suffix == "ms" || suffix == "s") {
            text += suffix;
        } else {
            diags.error(file, {l, c}, "invalid number suffix '" + suffix + "' (did you forget a space or a '.'?)");
        }
    }
    push(isFloat ? T::Float : T::Int, text, start, l, c);
}

void Lexer::lexIdent() {
    size_t start = pos;
    int l = line, c = col;
    while (std::isalnum((unsigned char)cur()) || cur() == '_' || (unsigned char)cur() >= 0x80) adv();
    std::string name = src.substr(start, pos - start);
    // Method names may end in '!' (in-place variants like list.sort!).
    if (cur() == '!' && at(1) != '=' && !out.empty() &&
        (out.back().kind == T::Dot || out.back().kind == T::KwFn)) {
        adv();
        name += '!';
    }
    auto it = keywords().find(name);
    if (it != keywords().end()) {
        push(it->second, name, start, l, c);
    } else {
        push(T::Ident, name, start, l, c);
    }
}

static void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
}

bool unescapeString(std::string_view raw, std::string& outStr, std::string& msg) {
    outStr.clear();
    outStr.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        char c = raw[i];
        if (c != '\\') {
            outStr += c;
            continue;
        }
        if (i + 1 >= raw.size()) {
            msg = "string ends with a lone backslash";
            return false;
        }
        char e = raw[++i];
        switch (e) {
            case 'n': outStr += '\n'; break;
            case 't': outStr += '\t'; break;
            case 'r': outStr += '\r'; break;
            case '0': outStr += '\0'; break;
            case '\\': outStr += '\\'; break;
            case '"': outStr += '"'; break;
            case '\'': outStr += '\''; break;
            case '{': outStr += '{'; break;
            case '}': outStr += '}'; break;
            case '\n': break;  // escaped newline: line continuation inside strings
            case 'x': {
                if (i + 2 >= raw.size() || !std::isxdigit((unsigned char)raw[i + 1]) ||
                    !std::isxdigit((unsigned char)raw[i + 2])) {
                    msg = "\\x must be followed by two hex digits";
                    return false;
                }
                outStr += (char)std::stoi(std::string(raw.substr(i + 1, 2)), nullptr, 16);
                i += 2;
                break;
            }
            case 'u': {
                if (i + 1 >= raw.size() || raw[i + 1] != '{') {
                    msg = "\\u must be written as \\u{HEX}";
                    return false;
                }
                size_t close = raw.find('}', i + 2);
                if (close == std::string_view::npos || close == i + 2 || close - (i + 2) > 6) {
                    msg = "malformed \\u{...} escape";
                    return false;
                }
                uint32_t cp = (uint32_t)std::stoul(std::string(raw.substr(i + 2, close - i - 2)), nullptr, 16);
                appendUtf8(outStr, cp);
                i = close;
                break;
            }
            default:
                msg = std::string("unknown escape sequence '\\") + e + "'";
                return false;
        }
    }
    return true;
}

void Lexer::lexString(char quote, bool fstring, bool rawString) {
    size_t start = pos;
    int l = line, c = col;
    if (fstring || rawString) adv();  // 'f' / 'r'
    bool triple = at(0) == quote && at(1) == quote && at(2) == quote;
    int qlen = triple ? 3 : 1;
    for (int i = 0; i < qlen; i++) adv();
    size_t bodyStart = pos;
    int braceDepth = 0;
    while (true) {
        char ch = cur();
        if (ch == '\0') {
            diags.error(file, {l, c}, "unterminated string literal");
            break;
        }
        if (!triple && (ch == '\n' || ch == '\r') && braceDepth == 0) {
            diags.error(file, {l, c}, "unterminated string literal (use \"\"\" for multi-line strings)");
            break;
        }
        if (ch == '\\' && !rawString) {
            if (at(1) == '{' || at(1) == '}') {
                std::string brace(1, at(1));
                diags.deprecated(file, {line, col},
                                 fstring ? "'\\" + brace + "' in an f-string is written '" + brace + brace + "'"
                                         : "'\\" + brace + "' is just '" + brace + "'",
                                 2, {{pos, pos + 2, fstring ? brace + brace : brace}});
            }
            adv();
            adv();
            continue;
        }
        if (fstring) {
            if (ch == '{') {
                if (braceDepth == 0 && at(1) == '{') {
                    adv();
                    adv();
                    continue;
                }
                braceDepth++;
            } else if (ch == '}') {
                if (braceDepth > 0) braceDepth--;
                else if (at(1) == '}') {
                    adv();
                    adv();
                    continue;
                }
            } else if (braceDepth > 0 && (ch == '"' || ch == '\'')) {
                // nested string literal inside an interpolation
                char q = ch;
                adv();
                while (cur() != '\0' && cur() != q && cur() != '\n') {
                    if (cur() == '\\') adv();
                    adv();
                }
                adv();
                continue;
            }
        }
        if (ch == quote && braceDepth == 0 && (!triple || (at(1) == quote && at(2) == quote))) break;
        adv();
    }
    size_t bodyEnd = pos;
    for (int i = 0; i < qlen && cur() != '\0'; i++) adv();
    std::string_view raw(src.data() + bodyStart, bodyEnd - bodyStart);
    if (fstring) {
        push(T::FStr, std::string(raw), start, l, c);
    } else if (rawString) {
        push(T::Str, std::string(raw), start, l, c);  // r"...": backslashes are just backslashes
    } else {
        std::string value, msg;
        if (!unescapeString(raw, value, msg)) diags.error(file, {l, c}, msg);
        push(T::Str, value, start, l, c);
    }
}

void Lexer::lexOperator() {
    size_t start = pos;
    int l = line, c = col;
    char a = cur(), b = at(1), d = at(2);
    auto emit = [&](T kind, int len) {
        for (int i = 0; i < len; i++) adv();
        push(kind, src.substr(start, len), start, l, c);
    };
    switch (a) {
        case '(':
            parenAfterFn.push_back(!out.empty() && out.back().kind == T::KwFn);
            bracketDepth++;
            return emit(T::LParen, 1);
        case '[':
            parenAfterFn.push_back(false);
            bracketDepth++;
            return emit(T::LBracket, 1);
        case '{':
            parenAfterFn.push_back(false);
            bracketDepth++;
            return emit(T::LBrace, 1);
        case ')':
        case ']':
        case '}': {
            // a block inside these brackets ends with them
            while (!islands.empty() && bracketDepth == islands.back().depth) closeIsland();
            bool fnParams = false;
            if (!parenAfterFn.empty()) {
                fnParams = parenAfterFn.back();
                parenAfterFn.pop_back();
            }
            if (bracketDepth > 0) bracketDepth--;
            emit(a == ')' ? T::RParen : a == ']' ? T::RBracket : T::RBrace, 1);
            lastCloseFnParams = a == ')' && fnParams;
            return;
        }
        case ',': return emit(T::Comma, 1);
        case '?': return emit(T::Question, 1);
        case '@': return emit(T::At, 1);
        case '$': return emit(T::Dollar, 1);
        case '~': return b == '/' ? emit(T::TildeSlash, 2) : emit(T::Tilde, 1);
        case '^': return emit(T::Caret, 1);
        case '+':
            if (b == '+') return emit(T::PlusPlus, 2);
            if (b == '=') return emit(T::PlusAssign, 2);
            return emit(T::Plus, 1);
        case '-':
            if (b == '>') return emit(T::Arrow, 2);
            if (b == '-') return emit(T::MinusMinus, 2);
            if (b == '=') return emit(T::MinusAssign, 2);
            return emit(T::Minus, 1);
        case '*':
            if (b == '*' && d == '=') return emit(T::StarStarAssign, 3);
            if (b == '*') return emit(T::StarStar, 2);
            if (b == '=') return emit(T::StarAssign, 2);
            return emit(T::Star, 1);
        case '/':
            if (b == '/') return emit(T::SlashSlash, 2);
            if (b == '=') return emit(T::SlashAssign, 2);
            return emit(T::Slash, 1);
        case '%':
            if (b == '=') return emit(T::PercentAssign, 2);
            return emit(T::Percent, 1);
        case '=':
            if (b == '=' && d == '=') return emit(T::EqEqEq, 3);
            if (b == '=') return emit(T::EqEq, 2);
            return emit(T::Assign, 1);
        case '!':
            if (b == '=' && d == '=') return emit(T::NotEqEq, 3);
            if (b == '=') return emit(T::NotEq, 2);
            return emit(T::Bang, 1);
        case '<':
            if (b == '=') return emit(T::LtEq, 2);
            if (b == '<') return emit(T::Shl, 2);
            if (b == ':') return emit(T::Recv, 2);
            return emit(T::Lt, 1);
        case '>':
            if (b == '=') return emit(T::GtEq, 2);
            if (b == '>') return emit(T::Shr, 2);
            return emit(T::Gt, 1);
        case ':':
            if (b == '>') return emit(T::Send, 2);
            return emit(T::Colon, 1);
        case '&':
            if (b == '&') return emit(T::AmpAmp, 2);
            return emit(T::Amp, 1);
        case '|':
            if (b == '|') return emit(T::PipePipe, 2);
            return emit(T::Pipe, 1);
        case '.':
            if (b == '.' && d == '<') return emit(T::DotDotLess, 3);
            if (b == '.' && d == '=') return emit(T::DotDotEq, 3);
            if (b == '.') return emit(T::DotDot, 2);
            return emit(T::Dot, 1);
        default: break;
    }
    adv();
    diags.error(file, {l, c}, std::string("unexpected character '") + a + "'");
}

std::vector<Token> Lexer::tokenize() {
    bool atLineStart = true;
    while (true) {
        if (atLineStart) {
            atLineStart = false;
            if (layoutActive()) {
                if (!handleLineStart()) break;
                if (layoutActive()) {
                    lineFirstTok = out.size();
                    lineStartDepth = bracketDepth;
                }
            }
        }
        while (cur() == ' ' || cur() == '\t' || cur() == '\f') {
            adv();
            spaceBefore = true;
        }
        char c = cur();
        if (c == '\0') break;
        if (c == '#') {
            size_t start = pos;
            int l = line, cl = col;
            while (cur() != '\0' && cur() != '\n' && cur() != '\r') adv();
            if (opts.keepTrivia) push(T::Comment, src.substr(start, pos - start), start, l, cl);
            continue;
        }
        if (c == '\n' || c == '\r') {
            size_t start = pos;
            int l = line, cl = col;
            if (c == '\r' && at(1) == '\n') adv();
            adv();
            bool openIsland = false;
            if (!layoutActive() && blockHeaderLine()) {
                int next = nextLineIndent();
                if (next > lineIndent) {
                    islands.push_back({bracketDepth, indents.size()});
                    indents.push_back(lineIndent);
                    openIsland = true;
                }
            }
            if (!layoutActive() && !openIsland) {
                // a new physical line inside brackets
                int w = 0;
                for (size_t p = pos; p < src.size() && (src[p] == ' ' || src[p] == '\t'); p++) w += src[p] == '\t' ? 4 : 1;
                if (pos < src.size() && src[pos] != '\n' && src[pos] != '\r') {
                    lineIndent = w;
                    lineFirstTok = out.size();
                    lineStartDepth = bracketDepth;
                }
            }
            if (layoutActive()) {
                bool needNewline = false;
                for (auto it = out.rbegin(); it != out.rend(); ++it) {
                    if (it->kind == T::Comment) continue;
                    needNewline = !isLayout(it->kind);
                    break;
                }
                if (needNewline) {
                    Token t;
                    t.kind = T::Newline;
                    t.line = l;
                    t.col = cl;
                    t.start = start;
                    t.end = start;
                    out.push_back(t);
                }
                atLineStart = true;
            }
            newlineBefore = true;
            spaceBefore = false;
            continue;
        }
        if (c == '\\' && (at(1) == '\n' || (at(1) == '\r' && at(2) == '\n'))) {
            adv();
            if (cur() == '\r') adv();
            adv();
            spaceBefore = true;
            continue;
        }
        if (c == 'f' && (at(1) == '"' || at(1) == '\'')) {
            lexString(at(1), true);
        } else if (c == 'r' && (at(1) == '"' || at(1) == '\'')) {
            lexString(at(1), false, true);
        } else if (std::isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) {
            lexIdent();
        } else if (std::isdigit((unsigned char)c)) {
            lexNumber();
        } else if (c == '"' || c == '\'') {
            lexString(c, false);
        } else {
            lexOperator();
        }
    }
    bool needNewline = false;
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
        if (it->kind == T::Comment) continue;
        needNewline = !isLayout(it->kind);
        break;
    }
    if (needNewline) {
        Token t;
        t.kind = T::Newline;
        t.line = line;
        t.col = col;
        t.start = t.end = pos;
        out.push_back(t);
    }
    while (indents.size() > 1) {
        indents.pop_back();
        Token t;
        t.kind = T::Dedent;
        t.line = line;
        t.col = col;
        t.start = t.end = pos;
        out.push_back(t);
    }
    Token eof;
    eof.kind = T::Eof;
    eof.line = line;
    eof.col = col;
    eof.start = eof.end = pos;
    eof.blankLinesBefore = blankLines;
    out.push_back(eof);
    return std::move(out);
}

}  // namespace ixo
