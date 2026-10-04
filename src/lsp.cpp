// Language server (LSP over stdio): diagnostics, hover, go to definition,
// completion (with snippets), document symbols, formatting, find references
// and rename (across the workspace), signature help, quick fixes, inlay hints
// and semantic highlighting.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <regex>
#include <memory>
#include <unordered_set>

#include "checker.hpp"
#include "json.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "program.hpp"
#include "tools.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace ixo {

namespace {

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string uriToPath(const std::string& uri) {
    if (uri.rfind("file://", 0) != 0) return uri;
    std::string p = percentDecode(uri.substr(7));
#ifdef _WIN32
    if (p.size() >= 3 && p[0] == '/' && p[2] == ':') p = p.substr(1);
    for (auto& c : p)
        if (c == '/') c = '\\';
#endif
    return p;
}

std::string pathToUri(const std::string& path) {
    std::string p = path;
    for (auto& c : p)
        if (c == '\\') c = '/';
    std::string enc;
    for (unsigned char c : p) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            enc += (char)c;
        else {
            char buf[4];
            snprintf(buf, sizeof buf, "%%%02X", c);
            enc += buf;
        }
    }
    if (!enc.empty() && enc[0] == '/') return "file://" + enc;
    return "file:///" + enc;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        std::string l = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

// byte column (0-based) -> UTF-16 code units
int toUtf16(const std::string& line, int byteCol) {
    int units = 0;
    for (int i = 0; i < byteCol && i < (int)line.size();) {
        unsigned char c = line[i];
        int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += len == 4 ? 2 : 1;
        i += len;
    }
    return units + std::max(0, byteCol - (int)line.size());
}

int fromUtf16(const std::string& line, int units) {
    int u = 0, i = 0;
    while (i < (int)line.size() && u < units) {
        unsigned char c = line[i];
        int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        u += len == 4 ? 2 : 1;
        i += len;
    }
    return i;
}

struct Doc {
    std::string uri, path, text;
    std::vector<std::string> lines;
    std::unique_ptr<Program> prog;
    ModuleUnit* unit = nullptr;
    std::vector<Diagnostic> fixable;  // this file's diagnostics that come with a fix
};

// What sits under the cursor.
struct Hit {
    enum K { None, Ident_, Member_, FnName, ClassName, ParamName, FieldName, Binding, TypeName } k = None;
    Node* node = nullptr;
    Ident* ident = nullptr;
    Member* member = nullptr;
    FnDecl* fn = nullptr;
    ClassDecl* cls = nullptr;
    Param* param = nullptr;
    FieldDecl* field = nullptr;
    Decl* decl = nullptr;
    std::string name;
};

bool inRange(SrcPos p, size_t len, int line, int col) {
    return p.line == line && col >= p.col && col < p.col + (int)std::max<size_t>(len, 1);
}

void findPattern(Pattern* p, int line, int col, Hit& h) {
    if (!p) return;
    if ((p->k == Pattern::Bind || p->k == Pattern::As) && inRange(p->pos, p->name.size(), line, col)) {
        h.k = Hit::Binding;
        h.decl = p->decl;
        h.name = p->name;
    }
    if (p->restDecl && inRange(p->restPos, p->restName.size(), line, col)) {
        h.k = Hit::Binding;
        h.decl = p->restDecl;
        h.name = p->restName;
    }
    for (auto& s : p->subs) findPattern(s.get(), line, col, h);
}

void findAt(Node* n, int line, int col, Hit& h) {
    if (!n) return;
    switch (n->kind) {
        case NK::Ident: {
            auto* id = static_cast<Ident*>(n);
            if (inRange(id->pos, id->name.size(), line, col)) {
                h.k = Hit::Ident_;
                h.ident = id;
                h.decl = id->decl;
                h.name = id->name;
            }
            break;
        }
        case NK::Member: {
            auto* m = static_cast<Member*>(n);
            if (inRange(m->namePos, m->name.size(), line, col)) {
                h.k = Hit::Member_;
                h.member = m;
                h.name = m->name;
            }
            break;
        }
        case NK::FnDecl: {
            auto* f = static_cast<FnDecl*>(n);
            if (!f->isLambda && inRange(f->namePos, f->name.size(), line, col)) {
                h.k = Hit::FnName;
                h.fn = f;
                h.decl = f->decl;
                h.name = f->name;
            }
            for (auto& p : f->params)
                if (inRange(p.pos, p.name.size(), line, col)) {
                    h.k = Hit::ParamName;
                    h.param = &p;
                    h.decl = p.decl;
                    h.name = p.name;
                }
            break;
        }
        case NK::ClassDecl: {
            auto* c = static_cast<ClassDecl*>(n);
            if (inRange(c->namePos, c->name.size() - (c->name.rfind('.') + 1), line, col)) {
                h.k = Hit::ClassName;
                h.cls = c;
                h.decl = c->decl;
                h.name = c->name;
            }
            for (auto& p : c->ctorParams)
                if (inRange(p.pos, p.name.size(), line, col)) {
                    h.k = Hit::ParamName;
                    h.param = &p;
                    h.cls = c;
                    h.decl = p.decl;
                    h.name = p.name;
                }
            for (auto& fd : c->fields)
                if (inRange(fd.pos, fd.name.size(), line, col)) {
                    h.k = Hit::FieldName;
                    h.field = &fd;
                    h.cls = c;
                    h.name = fd.name;
                }
            break;
        }
        case NK::For:
            for (auto& v : static_cast<For*>(n)->vars)
                if (inRange(v.pos, v.name.size(), line, col)) {
                    h.k = Hit::Binding;
                    h.decl = v.decl;
                    h.name = v.name;
                }
            break;
        case NK::ForRecv:
            for (auto& v : static_cast<ForRecv*>(n)->vars)
                if (inRange(v.pos, v.name.size(), line, col)) {
                    h.k = Hit::Binding;
                    h.decl = v.decl;
                    h.name = v.name;
                }
            break;
        case NK::Catch: {
            auto* c = static_cast<Catch*>(n);
            if (!c->errName.empty() && inRange(c->errPos, c->errName.size(), line, col)) {
                h.k = Hit::Binding;
                h.decl = c->errDecl;
                h.name = c->errName;
            }
            break;
        }
        case NK::Match:
            for (auto& arm : static_cast<Match*>(n)->arms) findPattern(arm.pat.get(), line, col, h);
            break;
        case NK::InterfaceDecl: {
            auto* x = static_cast<InterfaceDecl*>(n);
            if (inRange(x->namePos, x->name.size(), line, col)) {
                h.k = Hit::TypeName;
                h.node = x;
                h.decl = x->decl;
                h.name = x->name;
            }
            break;
        }
        case NK::EnumDecl: {
            auto* x = static_cast<EnumDecl*>(n);
            if (inRange(x->namePos, x->name.size(), line, col)) {
                h.k = Hit::TypeName;
                h.node = x;
                h.decl = x->decl;
                h.name = x->name;
            }
            break;
        }
        default: break;
    }
    forEachChild(n, [&](Node* c) { findAt(c, line, col, h); });
}

void collectIdents(Node* n, const std::string& name, std::vector<Ident*>& out) {
    if (!n) return;
    if (n->kind == NK::Ident && static_cast<Ident*>(n)->name == name) out.push_back(static_cast<Ident*>(n));
    forEachChild(n, [&](Node* c) { collectIdents(c, name, out); });
}

std::string paramList(const std::vector<Param>& params, size_t skip = 0) {
    std::string s;
    for (size_t i = skip; i < params.size(); i++) {
        if (i > skip) s += ", ";
        s += params[i].name;
        if (params[i].type) s += ": " + params[i].type->str();
        if (params[i].def) s += " = ...";
    }
    return s;
}

std::string fnSignature(FnDecl* f, Type* t, bool showExport = true) {
    std::string s = "fn " + f->name + (f->exported && showExport ? "*" : "");
    if (f->hasParamList || f->params.size() > (f->selfFirst ? 1u : 0u))
        s += "(" + paramList(f->params, f->selfFirst ? 1 : 0) + ")";
    if (f->ret)
        s += ": " + f->ret->str();
    else if (t && t->k == Type::Fn && t->ret && t->ret->k != Type::Nil && t->ret->k != Type::Any)
        s += ": " + typeToString(t->ret);
    return s;
}

std::string classSignature(ClassDecl* c) {
    std::string s = "class " + c->name + (c->exported ? "*" : "");
    if (c->hasCtorParams) s += "(" + paramList(c->ctorParams) + ")";
    if (c->info && !c->info->fields.empty()) {
        s += "\n    # fields: ";
        for (size_t i = 0; i < c->info->fields.size(); i++) s += (i ? ", " : "") + c->info->fields[i];
    }
    return s;
}

std::string interfaceSignature(InterfaceDecl* d) {
    std::string s = "interface " + d->name + (d->exported ? "*" : "");
    for (auto& m : d->methods) s += "\n    " + fnSignature(m.get(), nullptr, false) + (m->hasBody ? "  # default" : "");
    return s;
}

std::string enumSignature(EnumDecl* e) {
    std::string s = "enum " + e->name + (e->exported ? "*" : "");
    for (auto& v : e->variants) {
        s += "\n    " + v.name;
        if (!v.unit) s += "(" + paramList(v.cls->ctorParams) + ")";
    }
    for (auto& m : e->methods) s += "\n    " + fnSignature(m.get(), nullptr, false);
    return s;
}

std::string declSignature(Node* n) {
    if (!n) return "";
    switch (n->kind) {
        case NK::FnDecl: return fnSignature(static_cast<FnDecl*>(n), nullptr);
        case NK::ClassDecl: return classSignature(static_cast<ClassDecl*>(n));
        case NK::InterfaceDecl: return interfaceSignature(static_cast<InterfaceDecl*>(n));
        case NK::EnumDecl: return enumSignature(static_cast<EnumDecl*>(n));
        default: return "";
    }
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

// What find-references and rename are about.
struct Target {
    enum K { None, Local, Global, Member } k = None;
    Decl* decl = nullptr;  // Local; Member: the constructor parameter, if it is one
    std::string file;      // Global: the defining module (canonical path)
    std::string name;
    std::string cls;       // Member: the class
    std::string clsFile;   // Member: the class's module (canonical path)
};

bool refersTo(Decl* dc, const Target& t, const std::string& canon) {
    if (!dc) return false;
    if (t.k == Target::Local || t.k == Target::Member) return t.decl && dc == t.decl;
    if (t.k != Target::Global) return false;
    if (dc->kind == DeclKind::Import)
        return dc->importFrom && !dc->importFrom->isNative && canonicalPath(dc->importFrom->path) == t.file &&
               dc->importName == t.name;
    return dc->isGlobal && dc->name == t.name && canon == t.file && dc->kind != DeclKind::Module &&
           dc->kind != DeclKind::Builtin;
}

using Refs = std::vector<std::pair<SrcPos, int>>;

void patternRefs(Pattern* p, const Target& t, const std::string& canon, Refs& out) {
    if (!p) return;
    if ((p->k == Pattern::Bind || p->k == Pattern::As) && p->name == t.name && refersTo(p->decl, t, canon))
        out.push_back({p->pos, (int)p->name.size()});
    if (p->restDecl && p->restName == t.name && refersTo(p->restDecl, t, canon))
        out.push_back({p->restPos, (int)p->restName.size()});
    for (auto& s : p->subs) patternRefs(s.get(), t, canon, out);
}

Type* unwrap(Type* t);

void collectRefs(Node* n, const Target& t, const std::string& canon, Refs& out) {
    if (!n) return;
    auto add = [&](SrcPos p, size_t len) { out.push_back({p, (int)len}); };
    switch (n->kind) {
        case NK::Ident: {
            auto* id = static_cast<Ident*>(n);
            if (id->name == t.name && refersTo(id->decl, t, canon)) add(id->pos, id->name.size());
            break;
        }
        case NK::Member: {
            auto* m = static_cast<Member*>(n);
            if (m->name != t.name) break;
            Type* r = unwrap(m->object->type);
            if (t.k == Target::Member) {
                if (r && (r->k == Type::Instance || r->k == Type::Class) && r->cls && r->cls->name == t.cls &&
                    (t.clsFile.empty() || !r->cls->module || canonicalPath(r->cls->module->path) == t.clsFile))
                    add(m->namePos, m->name.size());
            } else if (t.k == Target::Global && r && r->k == Type::Module && r->module) {
                if (canonicalPath(r->module->path) == t.file) add(m->namePos, m->name.size());
            } else if (refersTo(m->ufcs, t, canon)) {
                add(m->namePos, m->name.size());
            }
            break;
        }
        case NK::FnDecl: {
            auto* f = static_cast<FnDecl*>(n);
            if (!f->isLambda && f->name == t.name && refersTo(f->decl, t, canon)) add(f->namePos, f->name.size());
            for (auto& p : f->params)
                if (p.name == t.name && refersTo(p.decl, t, canon)) add(p.pos, p.name.size());
            break;
        }
        case NK::ClassDecl: {
            auto* c = static_cast<ClassDecl*>(n);
            if (c->name == t.name && refersTo(c->decl, t, canon)) add(c->namePos, c->name.size());
            if (t.k == Target::Member && c->name == t.cls && (t.clsFile.empty() || canon == t.clsFile)) {
                for (auto& m : c->methods)
                    if (m->name == t.name) add(m->namePos, m->name.size());
                for (auto& fd : c->fields)
                    if (fd.name == t.name) add(fd.pos, fd.name.size());
                for (auto& p : c->ctorParams)
                    if (p.name == t.name) add(p.pos, p.name.size());
            } else {
                for (auto& p : c->ctorParams)
                    if (p.name == t.name && refersTo(p.decl, t, canon)) add(p.pos, p.name.size());
            }
            break;
        }
        case NK::InterfaceDecl: {
            auto* x = static_cast<InterfaceDecl*>(n);
            if (x->name == t.name && refersTo(x->decl, t, canon)) add(x->namePos, x->name.size());
            break;
        }
        case NK::EnumDecl: {
            auto* x = static_cast<EnumDecl*>(n);
            if (x->name == t.name && refersTo(x->decl, t, canon)) add(x->namePos, x->name.size());
            break;
        }
        case NK::For:
            for (auto& v : static_cast<For*>(n)->vars)
                if (v.name == t.name && refersTo(v.decl, t, canon)) add(v.pos, v.name.size());
            break;
        case NK::ForRecv:
            for (auto& v : static_cast<ForRecv*>(n)->vars)
                if (v.name == t.name && refersTo(v.decl, t, canon)) add(v.pos, v.name.size());
            break;
        case NK::Catch: {
            auto* c = static_cast<Catch*>(n);
            if (c->errName == t.name && refersTo(c->errDecl, t, canon)) add(c->errPos, c->errName.size());
            break;
        }
        case NK::Use: {
            auto* u = static_cast<Use*>(n);
            if (t.k == Target::Global && u->module && !u->module->isNative && canonicalPath(u->module->path) == t.file)
                for (auto& it : u->items)
                    if (it.name == t.name) add(it.pos, it.name.size());
            break;
        }
        case NK::Match:
            for (auto& arm : static_cast<Match*>(n)->arms) patternRefs(arm.pat.get(), t, canon, out);
            break;
        default: break;
    }
    forEachChild(n, [&](Node* c) { collectRefs(c, t, canon, out); });
}

Tag tagForType(Type* t) {
    if (!t) return Tag::COUNT;
    switch (t->k) {
        case Type::Int: return Tag::Int;
        case Type::Float: case Type::Num: return Tag::Float;
        case Type::Str: return Tag::Str;
        case Type::List: case Type::Iter: return Tag::List;
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
        default: return Tag::COUNT;
    }
}

Type* unwrap(Type* t) {
    while (t && (t->k == Type::Optional || t->k == Type::Result)) t = t->args[0];
    return t;
}

const char* kKeywords[] = {"fn", "class", "if", "elif", "else", "while", "for", "in", "not", "and", "or", "is",
                           "return", "match", "use", "var", "with", "fork", "true", "false", "nil", "break",
                           "continue", "retry", "self", "as", "todo", "tasks", "par", "defer", "interface", "enum"};

bool isKeyword(const std::string& s) {
    for (const char* k : kKeywords)
        if (s == k) return true;
    return s == "null";
}

class Server {
public:
    int run() {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        Json msg;
        while (readMessage(msg)) {
            try {
                handle(msg);
            } catch (const std::exception& e) {
                if (const Json* id = msg.get("id")) replyError(*id, -32603, std::string("internal error: ") + e.what());
            } catch (const PanicError& e) {
                if (const Json* id = msg.get("id")) replyError(*id, -32603, "internal error: " + e.msg);
            }
            if (exitNow) break;
        }
        return shutdownRequested ? 0 : 1;
    }

private:
    std::unordered_map<std::string, Doc> docs;
    bool shutdownRequested = false, exitNow = false;

    bool readMessage(Json& out) {
        size_t length = 0;
        std::string line;
        while (true) {
            line.clear();
            int c;
            while ((c = std::fgetc(stdin)) != EOF && c != '\n') line += (char)c;
            if (c == EOF) return false;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (line.rfind("Content-Length:", 0) == 0) length = (size_t)std::stoul(line.substr(15));
        }
        std::string body(length, '\0');
        size_t got = std::fread(body.data(), 1, length, stdin);
        if (got != length) return false;
        std::string err;
        out = Json::parse(body, &err);
        return true;
    }

    void send(const Json& msg) {
        std::string body = msg.dump();
        std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        std::fwrite(header.data(), 1, header.size(), stdout);
        std::fwrite(body.data(), 1, body.size(), stdout);
        std::fflush(stdout);
    }
    void reply(const Json& id, Json result) {
        Json m = Json::object();
        m.set("jsonrpc", "2.0");
        m.set("id", id);
        m.set("result", std::move(result));
        send(m);
    }
    void replyError(const Json& id, int code, const std::string& message) {
        Json m = Json::object();
        m.set("jsonrpc", "2.0");
        m.set("id", id);
        Json e = Json::object();
        e.set("code", code);
        e.set("message", message);
        m.set("error", e);
        send(m);
    }
    void notify(const std::string& method, Json params) {
        Json m = Json::object();
        m.set("jsonrpc", "2.0");
        m.set("method", method);
        m.set("params", std::move(params));
        send(m);
    }

    Json position(const Doc& d, int line, int byteCol) {
        Json p = Json::object();
        p.set("line", line);
        const std::string& l = line >= 0 && line < (int)d.lines.size() ? d.lines[line] : std::string();
        p.set("character", toUtf16(l, byteCol));
        return p;
    }
    Json range(const Doc& d, SrcPos p, int len) {
        Json r = Json::object();
        int line = std::max(0, p.line - 1), col = std::max(0, p.col - 1);
        r.set("start", position(d, line, col));
        r.set("end", position(d, line, col + std::max(len, 1)));
        return r;
    }
    Json rangeIn(const std::string& file, SrcPos p, int len) {
        // a position in another file: read it to convert columns
        Doc tmp;
        bool ok = false;
        std::string text;
        auto it = std::find_if(docs.begin(), docs.end(), [&](auto& kv) { return kv.second.path == file; });
        if (it != docs.end())
            text = it->second.text;
        else
            text = readFile(file, &ok);
        tmp.lines = splitLines(text);
        return range(tmp, p, len);
    }

    void analyze(Doc& d) {
        d.lines = splitLines(d.text);
        d.prog = std::make_unique<Program>();
        for (auto& [uri, other] : docs) d.prog->overlays[canonicalPath(other.path)] = other.text;
        d.prog->opts.compile = false;
        d.prog->opts.alwaysCheck = true;
        Diagnostics diags;
        d.unit = d.prog->loadSource("<main>", d.path, d.text, diags, d.prog->opts);
        Json list = Json::array();
        std::string canon = canonicalPath(d.path);
        d.fixable.clear();
        for (auto& diag : diags.list) {
            bool here = diag.file == d.path || canonicalPath(diag.file) == canon;
            if (here && !diag.fix.empty()) d.fixable.push_back(diag);
            Json item = Json::object();
            if (here) {
                item.set("range", range(d, diag.pos, diag.length));
                item.set("message", diag.message);
            } else {
                // a problem in an imported module: show it on the first line
                item.set("range", range(d, SrcPos{1, 1}, 1));
                item.set("message", diag.file + ":" + std::to_string(diag.pos.line) + ": " + diag.message);
            }
            item.set("severity", diag.severity == Diagnostic::Error ? 1 : 2);
            item.set("source", "ixo");
            list.push(item);
        }
        Json params = Json::object();
        params.set("uri", d.uri);
        params.set("diagnostics", list);
        notify("textDocument/publishDiagnostics", params);
    }

    Doc* docFor(const Json& params) {
        const Json* uri = params.at("textDocument", "uri");
        if (!uri) return nullptr;
        auto it = docs.find(uri->s);
        return it == docs.end() ? nullptr : &it->second;
    }

    bool cursor(const Doc& d, const Json& params, int& line, int& col) {
        const Json* pos = params.get("position");
        if (!pos) return false;
        int l = (int)pos->get("line")->num();
        int u = (int)pos->get("character")->num();
        line = l + 1;
        col = fromUtf16(l < (int)d.lines.size() ? d.lines[l] : std::string(), u) + 1;
        return true;
    }

    Hit hitAt(Doc& d, int line, int col) {
        Hit h;
        if (!d.unit || !d.unit->ast) return h;
        for (auto& s : d.unit->ast->stmts) findAt(s.get(), line, col, h);
        return h;
    }

    std::string builtinDoc(VM& vm, const std::string& name) {
        int idx = vm.builtinIndex(name);
        if (idx < 0) return "";
        auto& b = vm.builtins[idx];
        std::string s = "```ixo\n" + name + (b.sig.empty() ? "" : ": " + b.sig) + "\n```";
        if (!b.doc.empty()) s += "\n" + b.doc;
        return s;
    }

    std::string hoverText(Doc& d, Hit& h) {
        std::string text = hoverCore(d, h);
        std::string doc = hoverDoc(d, h);
        if (!doc.empty() && !text.empty()) text += "\n\n---\n\n" + doc;
        return text;
    }

    // The documentation of the declaration under the cursor.
    std::string hoverDoc(Doc& d, Hit& h) {
        switch (h.k) {
            case Hit::FnName: return docOf(h.fn, d.path);
            case Hit::ClassName: return docOf(h.cls, d.path);
            case Hit::TypeName: return docOf(h.node, d.path);
            case Hit::Ident_: {
                Decl* dc = h.decl;
                if (!dc) return "";
                if (dc->kind == DeclKind::Import && dc->importFrom && !dc->importFrom->isNative) {
                    auto g = dc->importFrom->globals.find(dc->importName);
                    if (g != dc->importFrom->globals.end()) return docOf(g->second->node, dc->importFrom->path);
                    return "";
                }
                if (dc->kind == DeclKind::Fn || dc->kind == DeclKind::Class || dc->kind == DeclKind::Interface ||
                    dc->kind == DeclKind::Enum)
                    return docOf(dc->node, dc->module && !dc->module->path.empty() ? dc->module->path : d.path);
                return "";
            }
            case Hit::Member_: {
                Type* recv = unwrap(h.member->object->type);
                if (recv && recv->k == Type::Instance && recv->cls) {
                    auto mi = recv->cls->methods.find(h.member->name);
                    if (mi != recv->cls->methods.end())
                        return docOf(mi->second, recv->cls->module ? recv->cls->module->path : d.path);
                }
                if (recv && recv->k == Type::Module && recv->module)
                    if (Decl* ex = recv->module->findExport(h.member->name)) return docOf(ex->node, recv->module->path);
                if (h.member->ufcs && h.member->ufcs->node)
                    return docOf(h.member->ufcs->node, d.path);
                return "";
            }
            default: return "";
        }
    }

    // Docstring of a declaration, or the `#` comment lines right above it.
    std::string docOf(Node* n, const std::string& file) {
        if (!n) return "";
        std::string doc;
        SrcPos at = n->pos;
        switch (n->kind) {
            case NK::FnDecl: doc = static_cast<FnDecl*>(n)->doc; break;
            case NK::ClassDecl: doc = static_cast<ClassDecl*>(n)->doc; break;
            case NK::InterfaceDecl: doc = static_cast<InterfaceDecl*>(n)->doc; break;
            case NK::EnumDecl: doc = static_cast<EnumDecl*>(n)->doc; break;
            default: return "";
        }
        if (!doc.empty()) return doc;
        const std::vector<std::string>* lines = nullptr;
        std::vector<std::string> tmp;
        auto it = std::find_if(docs.begin(), docs.end(),
                               [&](auto& kv) { return canonicalPath(kv.second.path) == canonicalPath(file); });
        if (it != docs.end()) {
            lines = &it->second.lines;
        } else {
            bool ok = false;
            std::string text = readFile(file, &ok);
            if (!ok) return "";
            tmp = splitLines(text);
            lines = &tmp;
        }
        std::vector<std::string> out;
        for (int l = at.line - 2; l >= 0 && l < (int)lines->size(); l--) {
            std::string s = (*lines)[l];
            size_t f = s.find_first_not_of(" \t");
            if (f == std::string::npos || s[f] != '#') break;
            if (s.compare(f, 2, "#!") == 0) break;
            s = s.substr(f + 1);
            if (!s.empty() && s[0] == ' ') s.erase(0, 1);
            out.push_back(s);
        }
        std::reverse(out.begin(), out.end());
        for (size_t i = 0; i < out.size(); i++) doc += (i ? "\n" : "") + out[i];
        return doc;
    }

    std::string hoverCore(Doc& d, Hit& h) {
        VM& vm = d.prog->vm;
        auto code = [](const std::string& s) { return "```ixo\n" + s + "\n```"; };
        switch (h.k) {
            case Hit::FnName: return code(fnSignature(h.fn, nullptr));
            case Hit::ClassName: return code(classSignature(h.cls));
            case Hit::TypeName: return code(declSignature(h.node));
            case Hit::FieldName: return code("field " + h.name + (h.field->type ? ": " + h.field->type->str() : ""));
            case Hit::ParamName:
                return code("param " + h.name + ": " + (h.param->type ? h.param->type->str() : std::string("any")));
            case Hit::Binding:
                return code(h.name + ": " + (h.decl && h.decl->type ? typeToString(h.decl->type) : std::string("any")));
            case Hit::Ident_: {
                Decl* dc = h.decl;
                if (!dc) return "";
                if (dc->kind == DeclKind::Builtin) return builtinDoc(vm, h.name);
                if (dc->kind == DeclKind::Fn && dc->node && dc->node->kind == NK::FnDecl)
                    return code(fnSignature(static_cast<FnDecl*>(dc->node), h.ident->type));
                if (dc->kind == DeclKind::Class && dc->node) return code(classSignature(static_cast<ClassDecl*>(dc->node)));
                if ((dc->kind == DeclKind::Interface || dc->kind == DeclKind::Enum) && dc->node)
                    return code(declSignature(dc->node));
                if (dc->kind == DeclKind::Module) return code("module " + (dc->importFrom ? dc->importFrom->name : h.name));
                if (dc->kind == DeclKind::Import && dc->importFrom) {
                    ModuleUnit* src = dc->importFrom;
                    if (src->isNative) {
                        auto it = vm.memberDocs.find(src->name + "." + dc->importName);
                        if (it != vm.memberDocs.end())
                            return code(src->name + "." + dc->importName + ": " + it->second.first) + "\n" + it->second.second;
                    }
                    auto g = src->globals.find(dc->importName);
                    if (g != src->globals.end() && g->second->node) {
                        if (g->second->node->kind == NK::FnDecl)
                            return code(fnSignature(static_cast<FnDecl*>(g->second->node), nullptr)) + "\nfrom module `" + src->name + "`";
                        std::string sig = declSignature(g->second->node);
                        if (!sig.empty()) return code(sig) + "\nfrom module `" + src->name + "`";
                    }
                    return code(h.name) + "\nfrom module `" + src->name + "`";
                }
                std::string kind = dc->kind == DeclKind::Param ? "param " : dc->kind == DeclKind::Self ? "" : "";
                std::string scope = dc->isGlobal ? " (module variable)" : "";
                return code(kind + h.name + ": " + typeToString(h.ident->type)) + scope;
            }
            case Hit::Member_: {
                Member* m = h.member;
                Type* recv = unwrap(m->object->type);
                if (recv && recv->k == Type::Instance && recv->cls) {
                    auto mi = recv->cls->methods.find(m->name);
                    if (mi != recv->cls->methods.end())
                        return code(fnSignature(mi->second, nullptr)) + "\nmethod of `" + recv->cls->name + "`";
                    if (recv->cls->fieldIndex.count(m->name))
                        return code(m->name + ": " + typeToString(m->type)) + "\nfield of `" + recv->cls->name + "`";
                }
                if (recv && recv->k == Type::Interface && recv->iface) {
                    auto mi = recv->iface->methods.find(m->name);
                    if (mi != recv->iface->methods.end())
                        return code(fnSignature(mi->second, nullptr, false)) + "\nmethod of the interface `" + recv->iface->name + "`";
                }
                if (recv && recv->k == Type::EnumDef && recv->en && recv->en->node)
                    for (auto& v : recv->en->node->variants)
                        if (v.name == m->name)
                            return code(recv->en->name + "." + v.name + (v.unit ? "" : "(" + paramList(v.cls->ctorParams) + ")")) +
                                   "\nvariant of `" + recv->en->name + "`";
                if (recv && recv->k == Type::Enum && recv->en && recv->en->node)
                    for (auto& fm : recv->en->node->methods)
                        if (fm->name == m->name)
                            return code(fnSignature(fm.get(), nullptr, false)) + "\nmethod of `" + recv->en->name + "`";
                if (recv && (recv->k == Type::Namespace || (recv->k == Type::Module && recv->module && recv->module->isNative))) {
                    auto it = vm.memberDocs.find(recv->name + "." + m->name);
                    if (it != vm.memberDocs.end())
                        return code(recv->name + "." + m->name + ": " + it->second.first) + "\n" + it->second.second;
                }
                if (recv && recv->k == Type::Module && recv->module) {
                    if (Decl* ex = recv->module->findExport(m->name))
                        if (ex->node && ex->node->kind == NK::FnDecl) return code(fnSignature(static_cast<FnDecl*>(ex->node), nullptr));
                }
                Tag tag = tagForType(recv);
                if (tag != Tag::COUNT) {
                    auto& table = vm.methods[(int)tag];
                    auto it = table.find(vm.sym(m->name));
                    if (it != table.end()) {
                        std::string s = code(typeToString(recv) + "." + m->name + ": " + it->second.sig);
                        if (!it->second.doc.empty()) s += "\n" + it->second.doc;
                        return s;
                    }
                }
                if (m->ufcs && m->ufcs->node && m->ufcs->node->kind == NK::FnDecl)
                    return code(fnSignature(static_cast<FnDecl*>(m->ufcs->node), nullptr)) + "\ncalled with the value as its first argument";
                if (m->ufcs && m->ufcs->kind == DeclKind::Builtin) return builtinDoc(vm, m->name);
                return code(m->name + ": " + typeToString(m->type));
            }
            default: return "";
        }
    }

    void onHover(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        int line, col;
        if (!d || !cursor(*d, params, line, col)) return reply(id, Json());
        Hit h = hitAt(*d, line, col);
        std::string text = hoverText(*d, h);
        if (text.empty()) return reply(id, Json());
        Json contents = Json::object();
        contents.set("kind", "markdown");
        contents.set("value", text);
        Json r = Json::object();
        r.set("contents", contents);
        reply(id, r);
    }

    Json location(const std::string& file, SrcPos p, int len) {
        Json loc = Json::object();
        loc.set("uri", pathToUri(file));
        loc.set("range", rangeIn(file, p, len));
        return loc;
    }

    void onDefinition(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        int line, col;
        if (!d || !cursor(*d, params, line, col)) return reply(id, Json());
        Hit h = hitAt(*d, line, col);
        Decl* dc = h.decl;
        if (h.k == Hit::Member_) {
            Member* m = h.member;
            Type* recv = unwrap(m->object->type);
            if (recv && recv->k == Type::Instance && recv->cls && recv->cls->node) {
                ClassDecl* c = recv->cls->node;
                std::string file = recv->cls->module ? recv->cls->module->path : d->path;
                auto mi = recv->cls->methods.find(m->name);
                if (mi != recv->cls->methods.end()) return reply(id, location(file, mi->second->namePos, (int)m->name.size()));
                for (auto& p : c->ctorParams)
                    if (p.name == m->name) return reply(id, location(file, p.pos, (int)p.name.size()));
                for (auto& fd : c->fields)
                    if (fd.name == m->name) return reply(id, location(file, fd.pos, (int)fd.name.size()));
                return reply(id, location(file, c->namePos, (int)c->name.size()));
            }
            if (recv && recv->k == Type::Module && recv->module) {
                if (Decl* ex = recv->module->findExport(m->name)) return reply(id, location(recv->module->path, ex->pos, (int)ex->name.size()));
            }
            dc = m->ufcs;
        }
        if (!dc || dc->kind == DeclKind::Builtin) return reply(id, Json());
        if (dc->kind == DeclKind::Import && dc->importFrom && !dc->importFrom->isNative) {
            auto g = dc->importFrom->globals.find(dc->importName);
            if (g != dc->importFrom->globals.end())
                return reply(id, location(dc->importFrom->path, g->second->pos, (int)g->second->name.size()));
        }
        if (dc->kind == DeclKind::Module && dc->importFrom && !dc->importFrom->isNative)
            return reply(id, location(dc->importFrom->path, SrcPos{1, 1}, 1));
        std::string file = dc->file.empty() ? d->path : dc->file;
        reply(id, location(file, dc->pos, (int)dc->name.size()));
    }

    Json item(const std::string& label, int kind, const std::string& detail, const std::string& doc = "") {
        Json it = Json::object();
        it.set("label", label);
        it.set("kind", kind);
        if (!detail.empty()) it.set("detail", detail);
        if (!doc.empty()) {
            Json md = Json::object();
            md.set("kind", "markdown");
            md.set("value", doc);
            it.set("documentation", md);
        }
        return it;
    }

    Type* receiverType(Doc& d, const std::string& lineText, int dotByte, int line) {
        // the identifier (or literal) right before the dot
        int e = dotByte;
        int s = e;
        while (s > 0 && (std::isalnum((unsigned char)lineText[s - 1]) || lineText[s - 1] == '_')) s--;
        std::string name = lineText.substr(s, e - s);
        if (name.empty()) {
            if (e > 0 && (lineText[e - 1] == '"' || lineText[e - 1] == '\'')) {
                static Type str;
                str.k = Type::Str;
                return &str;
            }
            return nullptr;
        }
        if (std::isdigit((unsigned char)name[0])) {
            static Type num;
            num.k = name.find('.') == std::string::npos ? Type::Int : Type::Float;
            return &num;
        }
        if (!d.unit || !d.unit->ast) return nullptr;
        std::vector<Ident*> ids;
        collectIdents(d.unit->ast.get(), name, ids);
        Ident* best = nullptr;
        for (Ident* id : ids) {
            if (!id->type || id->type->k == Type::Any) continue;
            bool before = id->pos.line < line || (id->pos.line == line && id->pos.col <= s + 1);
            if (!best || (before && (id->pos.line > best->pos.line || (id->pos.line == best->pos.line && id->pos.col > best->pos.col))))
                best = id;
        }
        return best ? best->type : nullptr;
    }

    void onCompletion(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        int line, col;
        Json items = Json::array();
        if (!d || !cursor(*d, params, line, col)) return reply(id, items);
        VM& vm = d->prog->vm;
        std::string text = line - 1 < (int)d->lines.size() ? d->lines[line - 1] : "";
        int c = std::min(col - 1, (int)text.size());
        int s = c;
        while (s > 0 && (std::isalnum((unsigned char)text[s - 1]) || text[s - 1] == '_' || text[s - 1] == '!')) s--;
        if (s > 0 && text[s - 1] == '.') {
            Type* t = unwrap(receiverType(*d, text, s - 1, line));
            if (!t) return reply(id, items);
            if (t->k == Type::Instance && t->cls) {
                for (auto& f : t->cls->fields) items.push(item(f, 5, "field of " + t->cls->name));
                for (auto& [n, fnd] : t->cls->methods)
                    if (isIdentifier(n))
                        items.push(item(n, 2, fnSignature(fnd, nullptr),
                                        docOf(fnd, t->cls->module ? t->cls->module->path : d->path)));
                return reply(id, items);
            }
            if (t->k == Type::Interface && t->iface) {
                for (auto& [n, fnd] : t->iface->methods)
                    if (isIdentifier(n)) items.push(item(n, 2, fnSignature(fnd, nullptr, false)));
                return reply(id, items);
            }
            if (t->k == Type::EnumDef && t->en && t->en->node) {
                for (auto& v : t->en->node->variants) items.push(item(v.name, 20, "variant of " + t->en->name));
                return reply(id, items);
            }
            if (t->k == Type::Enum && t->en && t->en->node) {
                for (auto& fm : t->en->node->methods)
                    if (isIdentifier(fm->name)) items.push(item(fm->name, 2, fnSignature(fm.get(), nullptr, false)));
                return reply(id, items);
            }
            if (t->k == Type::Namespace || (t->k == Type::Module && t->module && t->module->isNative)) {
                int idx = vm.builtinIndex(t->name);
                if (idx >= 0 && isObjType(vm.builtins[idx].value, OT::Namespace))
                    for (auto& [n, v] : as<ObjNamespace>(vm.builtins[idx].value)->members) {
                        if (n.empty() || n[0] == '$') continue;
                        auto doc = vm.memberDocs.find(t->name + "." + n);
                        bool isFn = isObjType(v, OT::Native) || isObjType(v, OT::Closure);
                        items.push(item(n, isFn ? 3 : 21, doc != vm.memberDocs.end() ? doc->second.first : "",
                                        doc != vm.memberDocs.end() ? doc->second.second : ""));
                    }
                return reply(id, items);
            }
            if (t->k == Type::Module && t->module) {
                for (auto& n : t->module->exportNames()) items.push(item(n, 3, "from " + t->module->name));
                return reply(id, items);
            }
            Tag tag = tagForType(t);
            if (tag != Tag::COUNT)
                for (auto& [sy, mi] : vm.methods[(int)tag]) items.push(item(vm.symName(sy), 2, mi.sig, mi.doc));
            return reply(id, items);
        }
        for (const char* k : kKeywords) items.push(item(k, 14, "keyword"));
        addSnippets(*d, line, items);
        for (auto& b : vm.builtins) {
            if (b.name.empty() || b.name[0] == '$') continue;
            bool ns = isObjType(b.value, OT::Namespace);
            items.push(item(b.name, ns ? 9 : 3, b.sig, b.doc));
        }
        if (d->unit) {
            for (Decl* g : d->unit->globalOrder) {
                int kind = g->kind == DeclKind::Fn          ? 3
                           : g->kind == DeclKind::Class     ? 7
                           : g->kind == DeclKind::Interface ? 8
                           : g->kind == DeclKind::Enum      ? 13
                           : g->kind == DeclKind::Module    ? 9
                                                            : 6;
                std::string gdoc = g->node ? docOf(g->node, d->path) : "";
                std::string detail = g->node ? declSignature(g->node) : "";
                if (detail.empty()) detail = g->type ? typeToString(g->type) : "";
                items.push(item(g->name, kind, detail, gdoc));
            }
            // locals of the function around the cursor
            for (auto& fi : d->unit->funcs) {
                FnDecl* f = fi.node;
                if (!f || f->pos.line > line || (f->endPos.line && f->endPos.line < line)) continue;
                for (Decl* p : fi.params) items.push(item(p->name, 6, p->type ? typeToString(p->type) : "param"));
                for (Decl* h : fi.hoisted) items.push(item(h->name, 6, h->type ? typeToString(h->type) : ""));
            }
        }
        reply(id, items);
    }

    Json symbol(Doc& d, const std::string& name, int kind, SrcPos pos, SrcPos namePos, SrcPos end) {
        Json s = Json::object();
        s.set("name", name);
        s.set("kind", kind);
        Json r = Json::object();
        r.set("start", position(d, pos.line - 1, pos.col - 1));
        int endLine = std::max(end.line, pos.line) - 1;
        r.set("end", position(d, endLine, endLine < (int)d.lines.size() ? (int)d.lines[endLine].size() : 0));
        s.set("range", r);
        s.set("selectionRange", range(d, namePos, (int)name.size()));
        return s;
    }

    void onSymbols(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        Json out = Json::array();
        if (!d || !d->unit || !d->unit->ast) return reply(id, out);
        std::unordered_set<std::string> seen;
        for (auto& st : d->unit->ast->stmts) {
            if (st->kind == NK::FnDecl) {
                auto* f = static_cast<FnDecl*>(st.get());
                out.push(symbol(*d, f->name, 12, f->pos, f->namePos, f->endPos));
            } else if (st->kind == NK::ClassDecl) {
                auto* c = static_cast<ClassDecl*>(st.get());
                SrcPos end = c->pos;
                for (auto& m : c->methods) end = m->endPos.line > end.line ? m->endPos : end;
                Json sym = symbol(*d, c->name, 5, c->pos, c->namePos, end);
                Json kids = Json::array();
                for (auto& m : c->methods) kids.push(symbol(*d, m->name, 6, m->pos, m->namePos, m->endPos));
                for (auto& fd : c->fields) kids.push(symbol(*d, fd.name, 8, fd.pos, fd.pos, fd.pos));
                sym.set("children", kids);
                out.push(sym);
            } else if (st->kind == NK::InterfaceDecl) {
                auto* x = static_cast<InterfaceDecl*>(st.get());
                SrcPos end = x->pos;
                for (auto& m : x->methods) end = m->endPos.line > end.line ? m->endPos : end;
                Json sym = symbol(*d, x->name, 11, x->pos, x->namePos, end);
                Json kids = Json::array();
                for (auto& m : x->methods) kids.push(symbol(*d, m->name, 6, m->pos, m->namePos, m->endPos));
                sym.set("children", kids);
                out.push(sym);
            } else if (st->kind == NK::EnumDecl) {
                auto* x = static_cast<EnumDecl*>(st.get());
                SrcPos end = x->pos;
                for (auto& v : x->variants) end = v.pos.line > end.line ? v.pos : end;
                for (auto& m : x->methods) end = m->endPos.line > end.line ? m->endPos : end;
                Json sym = symbol(*d, x->name, 10, x->pos, x->namePos, end);
                Json kids = Json::array();
                for (auto& v : x->variants) kids.push(symbol(*d, v.name, 22, v.pos, v.pos, v.pos));
                for (auto& m : x->methods) kids.push(symbol(*d, m->name, 6, m->pos, m->namePos, m->endPos));
                sym.set("children", kids);
                out.push(sym);
            } else if (st->kind == NK::Assign) {
                auto* a = static_cast<Assign*>(st.get());
                if (a->target->kind == NK::Ident && a->op == BinOp::None) {
                    auto* id2 = static_cast<Ident*>(a->target.get());
                    if (seen.insert(id2->name).second) out.push(symbol(*d, id2->name, 13, a->pos, id2->pos, a->pos));
                }
            }
        }
        reply(id, out);
    }

    // ---------------------------------------------------------- references / rename

    // Every parsed Ixo module we can see: open documents first, then what they import.
    std::vector<ModuleUnit*> allUnits(Doc& cur) {
        std::vector<ModuleUnit*> out;
        std::unordered_set<std::string> seen;
        auto add = [&](ModuleUnit* u) {
            if (!u || !u->ast || u->isNative || u->path.empty() || u->path[0] == '<') return;
            if (seen.insert(canonicalPath(u->path)).second) out.push_back(u);
        };
        add(cur.unit);
        for (auto& [uri, d] : docs) add(d.unit);
        for (auto& [uri, d] : docs)
            if (d.prog)
                for (auto& [p, u] : d.prog->byPath) add(u);
        for (const std::string& f : workspaceFiles()) {
            if (seen.count(canonicalPath(f))) continue;
            add(workspaceUnit(f));
        }
        return out;
    }

    // ---------------------------------------------------------- workspace index

    std::vector<std::string> roots;  // workspace folders
    struct WsFile {
        std::filesystem::file_time_type mtime;
        std::unique_ptr<Program> prog;
        ModuleUnit* unit = nullptr;
    };
    std::unordered_map<std::string, WsFile> wsFiles;  // canonical path -> parsed and checked

    std::vector<std::string> workspaceFiles() {
        namespace fs = std::filesystem;
        std::vector<std::string> out;
        static const std::unordered_set<std::string> skip = {".git", "node_modules", "build", "archive", ".vscode",
                                                             "out", "dist", "ixo_modules"};
        for (auto& root : roots) {
            std::error_code ec;
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                if (it->is_directory(ec) && skip.count(it->path().filename().string())) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->path().extension() == ".ixo") out.push_back(it->path().string());
                if (out.size() >= 3000) return out;
            }
        }
        return out;
    }

    ModuleUnit* workspaceUnit(const std::string& path) {
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(path, ec);
        std::string canon = canonicalPath(path);
        auto it = wsFiles.find(canon);
        if (it != wsFiles.end() && it->second.mtime == mtime) return it->second.unit;
        WsFile w;
        w.mtime = mtime;
        w.prog = std::make_unique<Program>();
        for (auto& [uri, other] : docs) w.prog->overlays[canonicalPath(other.path)] = other.text;
        w.prog->opts.compile = false;
        w.prog->opts.alwaysCheck = true;
        Diagnostics diags;
        w.unit = w.prog->loadFile(path, diags, w.prog->opts);
        ModuleUnit* u = w.unit;
        wsFiles[canon] = std::move(w);
        return u;
    }

    Target targetOf(Doc& d, Hit& h) {
        Target t;
        t.name = h.name;
        auto fromDecl = [&](Decl* dc) {
            if (!dc || dc->kind == DeclKind::Builtin || dc->kind == DeclKind::Module || dc->kind == DeclKind::Self ||
                dc->kind == DeclKind::Hidden)
                return;
            if (dc->kind == DeclKind::Import) {
                if (!dc->importFrom || dc->importFrom->isNative || dc->name != dc->importName) return;
                t.k = Target::Global;
                t.file = canonicalPath(dc->importFrom->path);
                return;
            }
            if (dc->isGlobal) {
                t.k = Target::Global;
                t.file = canonicalPath(dc->module && !dc->module->path.empty() ? dc->module->path : d.path);
                return;
            }
            t.k = Target::Local;
            t.decl = dc;
        };
        switch (h.k) {
            case Hit::Member_: {
                Member* m = h.member;
                Type* r = unwrap(m->object->type);
                if (r && (r->k == Type::Instance || r->k == Type::Class) && r->cls && r->cls->node &&
                    (r->cls->fieldIndex.count(m->name) || r->cls->methods.count(m->name))) {
                    t.k = Target::Member;
                    t.cls = r->cls->name;
                    if (r->cls->module) t.clsFile = canonicalPath(r->cls->module->path);
                    for (auto& p : r->cls->node->ctorParams)
                        if (p.name == m->name) t.decl = p.decl;
                } else if (r && r->k == Type::Module && r->module && !r->module->isNative) {
                    t.k = Target::Global;
                    t.file = canonicalPath(r->module->path);
                } else {
                    fromDecl(m->ufcs);
                }
                break;
            }
            case Hit::FieldName:
                t.k = Target::Member;
                t.cls = h.cls->name;
                t.clsFile = canonicalPath(d.path);
                break;
            case Hit::ParamName:
                if (h.cls) {
                    t.k = Target::Member;
                    t.cls = h.cls->name;
                    t.clsFile = canonicalPath(d.path);
                    t.decl = h.param->decl;
                } else {
                    fromDecl(h.decl);
                }
                break;
            case Hit::FnName:
                if (!h.decl && d.unit) {
                    // a method: find its class
                    for (auto& ci : d.unit->classes) {
                        auto it = ci.methods.find(h.fn->name);
                        if (it != ci.methods.end() && it->second == h.fn && ci.node) {
                            t.k = Target::Member;
                            t.cls = ci.name;
                            t.clsFile = canonicalPath(d.path);
                        }
                    }
                    break;
                }
                fromDecl(h.decl);
                break;
            case Hit::ClassName:
                if (h.cls && h.cls->name.find('.') != std::string::npos) break;  // enum variants
                fromDecl(h.decl);
                break;
            case Hit::Ident_:
            case Hit::Binding:
            case Hit::TypeName: fromDecl(h.decl); break;
            default: break;
        }
        if (t.name == "self" || t.name.empty()) t.k = Target::None;
        return t;
    }

    // file -> occurrences
    std::vector<std::pair<std::string, Refs>> findRefs(Doc& d, const Target& t) {
        std::vector<std::pair<std::string, Refs>> out;
        for (ModuleUnit* u : allUnits(d)) {
            if (t.k == Target::Local && u != d.unit) continue;
            Refs refs;
            std::string canon = canonicalPath(u->path);
            for (auto& s : u->ast->stmts) collectRefs(s.get(), t, canon, refs);
            std::sort(refs.begin(), refs.end(), [](auto& a, auto& b) {
                return a.first.line != b.first.line ? a.first.line < b.first.line : a.first.col < b.first.col;
            });
            refs.erase(std::unique(refs.begin(), refs.end(),
                                   [](auto& a, auto& b) { return a.first.line == b.first.line && a.first.col == b.first.col; }),
                       refs.end());
            if (!refs.empty()) out.push_back({u->path, refs});
        }
        return out;
    }

    void onReferences(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        int line, col;
        Json out = Json::array();
        if (!d || !cursor(*d, params, line, col)) return reply(id, out);
        Hit h = hitAt(*d, line, col);
        Target t = targetOf(*d, h);
        if (t.k == Target::None) return reply(id, out);
        for (auto& [file, refs] : findRefs(*d, t))
            for (auto& [pos, len] : refs) out.push(location(file, pos, len));
        reply(id, out);
    }

    void onPrepareRename(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        int line, col;
        if (!d || !cursor(*d, params, line, col)) return reply(id, Json());
        Hit h = hitAt(*d, line, col);
        Target t = targetOf(*d, h);
        if (t.k == Target::None) return replyError(id, -32803, "this can't be renamed");
        // the occurrence under the cursor
        for (auto& [file, refs] : findRefs(*d, t)) {
            if (canonicalPath(file) != canonicalPath(d->path)) continue;
            for (auto& [pos, len] : refs)
                if (inRange(pos, len, line, col)) {
                    Json r = Json::object();
                    r.set("range", range(*d, pos, len));
                    r.set("placeholder", t.name);
                    return reply(id, r);
                }
        }
        replyError(id, -32803, "this can't be renamed");
    }

    void onRename(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        int line, col;
        if (!d || !cursor(*d, params, line, col)) return reply(id, Json());
        const Json* nn = params.get("newName");
        std::string newName = nn ? nn->s : "";
        if (!isIdentifier(newName) || isKeyword(newName))
            return replyError(id, -32602, "'" + newName + "' is not a valid name");
        Hit h = hitAt(*d, line, col);
        Target t = targetOf(*d, h);
        if (t.k == Target::None) return replyError(id, -32803, "this can't be renamed");
        Json changes = Json::object();
        for (auto& [file, refs] : findRefs(*d, t)) {
            Json edits = Json::array();
            for (auto& [pos, len] : refs) {
                Json e = Json::object();
                e.set("range", rangeIn(file, pos, len));
                e.set("newText", newName);
                edits.push(e);
            }
            auto open = std::find_if(docs.begin(), docs.end(),
                                     [&](auto& kv) { return canonicalPath(kv.second.path) == canonicalPath(file); });
            changes.set(open != docs.end() ? open->second.uri : pathToUri(file), edits);
        }
        Json edit = Json::object();
        edit.set("changes", changes);
        reply(id, edit);
    }

    // ---------------------------------------------------------- signature help

    // A signature "fn(a, b: int) -> x" as LSP SignatureInformation with parameter offsets.
    Json signatureInfo(const std::string& label, size_t open, const std::string& doc = "") {
        Json sig = Json::object();
        sig.set("label", label);
        if (!doc.empty()) {
            Json md = Json::object();
            md.set("kind", "markdown");
            md.set("value", doc);
            sig.set("documentation", md);
        }
        Json ps = Json::array();
        if (open != std::string::npos) {
            int depth = 0;
            size_t start = open + 1;
            auto push = [&](size_t a, size_t b) {
                while (a < b && label[a] == ' ') a++;
                if (a >= b) return;
                Json p = Json::object();
                Json off = Json::array();
                off.push((int)toUtf16(label, (int)a));
                off.push((int)toUtf16(label, (int)b));
                p.set("label", off);
                ps.push(p);
            };
            for (size_t i = open + 1; i < label.size(); i++) {
                char c = label[i];
                if (c == '(' || c == '[' || c == '{') depth++;
                else if ((c == ')' || c == ']' || c == '}') && depth > 0) depth--;
                else if (c == ')' && depth == 0) {
                    push(start, i);
                    break;
                } else if (c == ',' && depth == 0) {
                    push(start, i);
                    start = i + 1;
                }
            }
        }
        sig.set("parameters", ps);
        return sig;
    }

    Json fnSigInfo(FnDecl* f, const std::string& shownName, const std::string& file = "") {
        std::string label = shownName + "(" + paramList(f->params, f->selfFirst ? 1 : 0) + ")";
        if (f->ret) label += ": " + f->ret->str();
        return signatureInfo(label, shownName.size(), file.empty() ? f->doc : docOf(f, file));
    }

    Json nativeSigInfo(const std::string& name, const std::string& sig, const std::string& doc) {
        // "fn(any...) -> str"  ->  "name(any...) -> str"
        std::string label = sig.rfind("fn(", 0) == 0 ? name + sig.substr(2) : name + "(" + sig + ")";
        return signatureInfo(label, name.size(), doc);
    }

    Json callableSig(Doc& d, Decl* dc, const std::string& name) {
        if (!dc) return Json();
        VM& vm = d.prog->vm;
        if (dc->kind == DeclKind::Builtin) {
            int idx = vm.builtinIndex(name);
            if (idx >= 0 && !vm.builtins[idx].sig.empty()) return nativeSigInfo(name, vm.builtins[idx].sig, vm.builtins[idx].doc);
            return Json();
        }
        Node* n = dc->node;
        if (dc->kind == DeclKind::Import && dc->importFrom) {
            if (dc->importFrom->isNative) {
                auto it = vm.memberDocs.find(dc->importFrom->name + "." + dc->importName);
                if (it != vm.memberDocs.end()) return nativeSigInfo(name, it->second.first, it->second.second);
                return Json();
            }
            auto g = dc->importFrom->globals.find(dc->importName);
            n = g != dc->importFrom->globals.end() ? g->second->node : nullptr;
        }
        if (!n) return Json();
        std::string nfile = dc->kind == DeclKind::Import && dc->importFrom ? dc->importFrom->path
                            : dc->module && !dc->module->path.empty() ? dc->module->path
                                                                     : d.path;
        if (n->kind == NK::FnDecl) return fnSigInfo(static_cast<FnDecl*>(n), name, nfile);
        if (n->kind == NK::ClassDecl) {
            auto* c = static_cast<ClassDecl*>(n);
            if (c->hasCtorParams) return signatureInfo(name + "(" + paramList(c->ctorParams) + ")", name.size());
            if (c->info && c->info->init) return fnSigInfo(c->info->init, name);
            return signatureInfo(name + "()", name.size());
        }
        return Json();
    }

    void onSignatureHelp(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        int line, col;
        if (!d || !cursor(*d, params, line, col) || !d->prog) return reply(id, Json());
        std::string text = line - 1 < (int)d->lines.size() ? d->lines[line - 1] : "";
        int c = std::min(col - 1, (int)text.size());
        // find the unclosed '(' before the cursor, counting the commas at its level
        int depth = 0, commas = 0, open = -1;
        bool inStr = false;
        char quote = 0;
        for (int i = 0; i < c; i++) {  // strings: scan forwards to know what's quoted
            char ch = text[i];
            if (inStr) {
                if (ch == '\\') i++;
                else if (ch == quote) inStr = false;
                continue;
            }
            if (ch == '"' || ch == '\'') {
                inStr = true;
                quote = ch;
            } else if (ch == '#') {
                break;
            }
        }
        if (inStr) return reply(id, Json());
        std::vector<int> stack;  // positions of open brackets
        std::vector<int> commaStack;
        inStr = false;
        for (int i = 0; i < c; i++) {
            char ch = text[i];
            if (inStr) {
                if (ch == '\\') i++;
                else if (ch == quote) inStr = false;
                continue;
            }
            if (ch == '"' || ch == '\'') {
                inStr = true;
                quote = ch;
            } else if (ch == '(' || ch == '[' || ch == '{') {
                stack.push_back(i);
                commaStack.push_back(0);
            } else if ((ch == ')' || ch == ']' || ch == '}') && !stack.empty()) {
                stack.pop_back();
                commaStack.pop_back();
            } else if (ch == ',' && !commaStack.empty()) {
                commaStack.back()++;
            }
        }
        (void)depth;
        for (int k = (int)stack.size() - 1; k >= 0; k--)
            if (text[stack[k]] == '(') {
                open = stack[k];
                commas = commaStack[k];
                break;
            }
        if (open < 0) return reply(id, Json());
        int e = open;
        while (e > 0 && text[e - 1] == ' ') e--;
        int s = e;
        while (s > 0 && (std::isalnum((unsigned char)text[s - 1]) || text[s - 1] == '_' || text[s - 1] == '!')) s--;
        std::string name = text.substr(s, e - s);
        if (name.empty() || isKeyword(name)) return reply(id, Json());
        VM& vm = d->prog->vm;
        Json sig;
        if (s > 0 && text[s - 1] == '.') {
            Type* t = unwrap(receiverType(*d, text, s - 1, line));
            if (t && t->k == Type::Instance && t->cls) {
                auto mi = t->cls->methods.find(name);
                if (mi != t->cls->methods.end()) sig = fnSigInfo(mi->second, name);
            } else if (t && t->k == Type::Interface && t->iface) {
                auto mi = t->iface->methods.find(name);
                if (mi != t->iface->methods.end()) sig = fnSigInfo(mi->second, name);
            } else if (t && (t->k == Type::Namespace || (t->k == Type::Module && t->module && t->module->isNative))) {
                auto it = vm.memberDocs.find(t->name + "." + name);
                if (it != vm.memberDocs.end()) sig = nativeSigInfo(name, it->second.first, it->second.second);
            } else if (t && t->k == Type::Module && t->module) {
                if (Decl* ex = t->module->findExport(name)) sig = callableSig(*d, ex, name);
            } else if (t) {
                Tag tag = tagForType(t);
                if (tag != Tag::COUNT) {
                    auto& table = vm.methods[(int)tag];
                    auto it = table.find(vm.sym(name));
                    if (it != table.end()) sig = nativeSigInfo(name, it->second.sig, it->second.doc);
                }
            }
        } else {
            // the nearest identifier with this name at or before the cursor tells us what it is
            std::vector<Ident*> ids;
            if (d->unit && d->unit->ast) collectIdents(d->unit->ast.get(), name, ids);
            Ident* best = nullptr;
            for (Ident* x : ids) {
                if (!x->decl) continue;
                bool before = x->pos.line < line || (x->pos.line == line && x->pos.col <= s + 1);
                if (before && (!best || x->pos.line > best->pos.line ||
                               (x->pos.line == best->pos.line && x->pos.col > best->pos.col)))
                    best = x;
            }
            Decl* dc = best ? best->decl : nullptr;
            if (!dc && d->unit) {
                auto g = d->unit->globals.find(name);
                if (g != d->unit->globals.end()) dc = g->second;
            }
            if (!dc) {
                int idx = vm.builtinIndex(name);
                if (idx >= 0 && !vm.builtins[idx].sig.empty()) sig = nativeSigInfo(name, vm.builtins[idx].sig, vm.builtins[idx].doc);
            } else {
                sig = callableSig(*d, dc, name);
            }
        }
        if (sig.isNull()) return reply(id, Json());
        Json sigs = Json::array();
        sigs.push(sig);
        Json r = Json::object();
        r.set("signatures", sigs);
        r.set("activeSignature", 0);
        r.set("activeParameter", commas);
        reply(id, r);
    }

    // ---------------------------------------------------------- snippets

    Json snippet(const std::string& label, const std::string& filter, const std::string& body, const std::string& detail) {
        Json it = Json::object();
        it.set("label", label);
        it.set("kind", 15);
        it.set("filterText", filter);
        it.set("insertText", body);
        it.set("insertTextFormat", 2);
        it.set("detail", detail);
        it.set("sortText", "~" + filter);  // after exact names
        return it;
    }

    // Match arms for every variant of an enum.
    std::string enumArms(EnumInfo* ei, const std::string& indent) {
        std::string s;
        if (!ei || !ei->node) return s;
        for (auto& v : ei->node->variants) {
            s += indent + ei->name + "." + v.name;
            if (!v.unit) {
                s += "(";
                for (size_t i = 0; i < v.cls->ctorParams.size(); i++) s += (i ? ", " : "") + v.cls->ctorParams[i].name;
                s += ")";
            }
            s += " -> todo\n";
        }
        return s;
    }

    static EnumInfo* enumOfType(Type* t) {
        t = unwrap(t);
        if (!t) return nullptr;
        if (t->k == Type::Enum) return t->en;
        if (t->k == Type::Instance && t->cls) return t->cls->enumOf;
        return nullptr;
    }

    void addSnippets(Doc& d, int line, Json& items) {
        items.push(snippet("fn name(params)", "fn", "fn ${1:name}(${2})\n\t$0", "function"));
        items.push(snippet("fn name = expr", "fn", "fn ${1:name}(${2}) = ${0:expr}", "one-line function"));
        items.push(snippet("class Name(fields)", "class", "class ${1:Name}(${2:fields})\n\tfn ${3:method}\n\t\t$0",
                           "class"));
        items.push(snippet("interface Name", "interface", "interface ${1:Name}\n\tfn ${2:method}$0", "interface"));
        items.push(snippet("enum Name", "enum", "enum ${1:Name}\n\t${2:First}\n\t${3:Second}$0", "enum"));
        items.push(snippet("match value", "match", "match ${1:value}\n\t${2:pattern} -> ${3:result}\n\t_ -> ${0:other}",
                           "match"));
        items.push(snippet("for x in items", "for", "for ${1:x} in ${2:items}\n\t$0", "loop"));
        items.push(snippet("while cond", "while", "while ${1:cond}\n\t$0", "loop"));
        items.push(snippet("if cond", "if", "if ${1:cond}\n\t$0", "condition"));
        items.push(snippet("@test fn", "test", "@test\nfn ${1:name}\n\tassert_eq(${2:actual}, ${3:expected})$0", "test"));
        items.push(snippet("defer", "defer", "defer ${0:cleanup}", "run at the end of the block"));
        // right after `match <name>`: every variant of its enum
        if (line >= 2 && line - 2 < (int)d.lines.size()) {
            int pl = line - 2;
            while (pl >= 0 && d.lines[pl].find_first_not_of(" \t") == std::string::npos) pl--;
            if (pl < 0) return;
            static const std::regex re(R"(\bmatch\s+([A-Za-z_][A-Za-z0-9_]*)\s*$)");
            std::smatch m;
            const std::string& prevLine = d.lines[pl];
            if (!std::regex_search(prevLine, m, re)) return;
            std::string subj = m[1].str();
            std::string lt = prevLine + ".";  // receiverType looks at the name before a dot
            Type* t = receiverType(d, lt, (int)prevLine.size(), pl + 1);
            EnumInfo* ei = enumOfType(t);
            if (!ei) return;
            std::string arms = enumArms(ei, "");
            if (!arms.empty() && arms.back() == '\n') arms.pop_back();
            Json it = Json::object();
            it.set("label", "all " + ei->name + " variants");
            it.set("kind", 15);
            it.set("filterText", "match " + ei->name + " " + subj);
            it.set("insertText", arms);
            it.set("detail", "one arm per variant");
            it.set("preselect", true);
            it.set("sortText", "!");
            items.push(it);
        }
    }

    // ---------------------------------------------------------- quick fixes

    static int indentOf(const std::string& s) {
        size_t f = s.find_first_not_of(" \t");
        return f == std::string::npos ? -1 : (int)f;
    }

    // Last line (0-based) of the indented block that starts after line `header`.
    int blockEnd(Doc& d, int header) {
        int base = indentOf(d.lines[header]);
        int last = header;
        for (int l = header + 1; l < (int)d.lines.size(); l++) {
            int in = indentOf(d.lines[l]);
            if (in < 0) continue;
            if (in <= base) break;
            last = l;
        }
        return last;
    }

    Json textEdit(Doc& d, int line, int col, int endLine, int endCol, const std::string& text) {
        Json e = Json::object();
        Json r = Json::object();
        r.set("start", position(d, line, col));
        r.set("end", position(d, endLine, endCol));
        e.set("range", r);
        e.set("newText", text);
        return e;
    }

    Json action(const std::string& title, const Json& diag, const std::string& uri, Json edits, bool preferred = false) {
        Json a = Json::object();
        a.set("title", title);
        a.set("kind", "quickfix");
        Json ds = Json::array();
        ds.push(diag);
        a.set("diagnostics", ds);
        Json changes = Json::object();
        changes.set(uri, std::move(edits));
        Json we = Json::object();
        we.set("changes", changes);
        a.set("edit", we);
        if (preferred) a.set("isPreferred", true);
        return a;
    }

    Json insertAt(Doc& d, int line0, int byteCol, const std::string& text) {
        Json edits = Json::array();
        edits.push(textEdit(d, line0, byteCol, line0, byteCol, text));
        return edits;
    }

    // Finds a node of kind `k` whose position is (line, col) (1-based).
    Node* nodeAt(Node* n, NK k, int line, int col) {
        if (!n) return nullptr;
        if (n->kind == k && n->pos.line == line && n->pos.col == col) return n;
        Node* found = nullptr;
        forEachChild(n, [&](Node* c) {
            if (!found) found = nodeAt(c, k, line, col);
        });
        return found;
    }
    Node* nodeAtAny(Doc& d, NK k, int line, int col) {
        if (!d.unit || !d.unit->ast) return nullptr;
        for (auto& s : d.unit->ast->stmts)
            if (Node* n = nodeAt(s.get(), k, line, col)) return n;
        return nullptr;
    }
    Member* memberAt(Doc& d, int line, int col) {
        Hit h = hitAt(d, line, col);
        return h.k == Hit::Member_ ? h.member : nullptr;
    }

    // A byte offset in the document as (line, byte column), both from 0.
    std::pair<int, int> lineColOf(const Doc& d, size_t off) {
        int line = 0;
        size_t lineStart = 0;
        for (size_t k = 0; k < off && k < d.text.size(); k++)
            if (d.text[k] == '\n') {
                line++;
                lineStart = k + 1;
            }
        return {line, (int)(off - lineStart)};
    }
    Json editsOf(Doc& d, const std::vector<TextEdit>& fix) {
        Json edits = Json::array();
        for (auto& e : fix) {
            auto [l1, c1] = lineColOf(d, e.start);
            auto [l2, c2] = lineColOf(d, e.end);
            edits.push(textEdit(d, l1, c1, l2, c2, e.text));
        }
        return edits;
    }

    void onCodeAction(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        Json out = Json::array();
        if (!d || !d->unit) return reply(id, out);
        const Json* diags = params.at("context", "diagnostics");
        if (!diags) return reply(id, out);
        bool anyOld = false;
        for (auto& diag : diags->arr) {
            std::string msg = diag.get("message") ? diag.get("message")->s : "";
            const Json* start = diag.at("range", "start");
            if (!start) continue;
            int l0 = (int)start->get("line")->num();
            if (l0 < 0 || l0 >= (int)d->lines.size()) continue;
            int line = l0 + 1;
            int col = fromUtf16(d->lines[l0], (int)start->get("character")->num()) + 1;
            // the diagnostic's own fix (old spellings, -5.abs, l.sort, ...)
            for (auto& f : d->fixable) {
                if (f.message != msg || f.pos.line != line || f.pos.col != col) continue;
                if (f.deprecated) anyOld = true;
                out.push(action(f.deprecated ? "Use the new spelling" : "Apply the suggested fix", diag, d->uri,
                                editsOf(*d, f.fix), true));
                break;
            }
            std::smatch m;
            // did you mean
            static const std::regex didYouMean(R"(\(did you mean '([^']+)'\?\))");
            if (std::regex_search(msg, m, didYouMean)) {
                const Json* end = diag.at("range", "end");
                Json edits = Json::array();
                Json e = Json::object();
                Json r = Json::object();
                r.set("start", *start);
                r.set("end", end ? *end : *start);
                e.set("range", r);
                e.set("newText", m[1].str());
                edits.push(e);
                out.push(action("Change to '" + m[1].str() + "'", diag, d->uri, edits, true));
            }
            // a class that doesn't implement its interfaces
            static const std::regex missingMethod(R"(^(\w+) doesn't define '([^']+)', which (\w+) requires)");
            if (std::regex_search(msg, m, missingMethod)) {
                auto* c = static_cast<ClassDecl*>(nodeAtAnyName(*d, line, col));
                if (c && c->info) {
                    int header = c->pos.line - 1;
                    int last = blockEnd(*d, header);
                    std::string ind(std::max(0, indentOf(d->lines[header])) + 4, ' ');
                    std::string text;
                    for (InterfaceInfo* ii : c->info->interfaces)
                        for (auto& r : ii->required) {
                            bool has = false;
                            for (auto& mm : c->methods)
                                if (mm->name == r) has = true;
                            if (has || text.find("fn " + r + "(") != std::string::npos ||
                                text.find("fn " + r + "\n") != std::string::npos)
                                continue;
                            // the same form as the interface: fn area (no parentheses) or fn area(x)
                            FnDecl* proto = ii->methods[r];
                            std::string params = proto->hasParamList ? "(" + paramList(proto->params) + ")" : "";
                            if (proto->ret) params += ": " + proto->ret->str();
                            text += "\n" + ind + "fn " + r + params + "\n" + ind + "    todo";
                        }
                    if (!text.empty())
                        out.push(action("Add the missing methods", diag, d->uri,
                                        insertAt(*d, last, (int)d->lines[last].size(), text), true));
                }
            }
            // a match that misses enum variants
            if (msg.rfind("this match doesn't handle ", 0) == 0) {
                auto* mt = static_cast<Match*>(nodeAtAny(*d, NK::Match, line, col));
                EnumInfo* ei = mt ? enumOfType(mt->subject->type) : nullptr;
                if (ei && ei->node) {
                    int last = blockEnd(*d, l0);
                    int armIndent = -1;
                    for (int l = l0 + 1; l <= last && armIndent < 0; l++) armIndent = indentOf(d->lines[l]);
                    if (armIndent < 0) armIndent = std::max(0, indentOf(d->lines[l0])) + 4;
                    std::string text;
                    std::string missing = msg.substr(26);
                    for (auto& v : ei->node->variants) {
                        std::string full = ei->name + "." + v.name;
                        bool listed = false;
                        size_t p = 0;
                        while ((p = missing.find(full, p)) != std::string::npos) {
                            size_t e2 = p + full.size();
                            if (e2 == missing.size() || missing[e2] == ',') listed = true;
                            p = e2;
                        }
                        if (!listed) continue;
                        text += "\n" + std::string(armIndent, ' ') + full;
                        if (!v.unit) {
                            text += "(";
                            for (size_t i = 0; i < v.cls->ctorParams.size(); i++)
                                text += (i ? ", " : "") + v.cls->ctorParams[i].name;
                            text += ")";
                        }
                        text += " -> todo";
                    }
                    if (!text.empty())
                        out.push(action("Add the missing match arms", diag, d->uri,
                                        insertAt(*d, last, (int)d->lines[last].size(), text), true));
                }
            }
            // an unknown name another module exports
            static const std::regex undefinedName(R"(^undefined name '([^']+)')");
            if (std::regex_search(msg, m, undefinedName)) {
                std::string name = m[1].str();
                namespace fs = std::filesystem;
                fs::path here = fs::path(d->path).parent_path();
                for (const std::string& f : workspaceFiles()) {
                    if (canonicalPath(f) == canonicalPath(d->path)) continue;
                    std::error_code ec;
                    fs::path rel = fs::relative(fs::path(f), here, ec);
                    if (ec || rel.empty() || rel.string().find("..") != std::string::npos) continue;
                    ModuleUnit* u = workspaceUnit(f);
                    if (!u || !u->findExport(name)) continue;
                    std::string dotted;
                    for (const auto& part : rel.replace_extension()) dotted += (dotted.empty() ? "" : ".") + part.string();
                    // after the last top-level `use`, else at the top (after leading comments)
                    int at = 0;
                    for (int l = 0; l < (int)d->lines.size(); l++) {
                        const std::string& s = d->lines[l];
                        if (s.rfind("use ", 0) == 0 || (s.rfind("in ", 0) == 0 && s.find(" use ") != std::string::npos))
                            at = l + 1;
                        else if (at == 0 && !s.empty() && s[0] == '#')
                            at = l + 1;
                    }
                    out.push(action("Import '" + name + "' from " + dotted, diag, d->uri,
                                    insertAt(*d, at, 0, "in " + dotted + " use " + name + "\n")));
                }
            }
            // private members: export them
            if (msg.find("mark it with *") != std::string::npos) {
                std::string file;
                SrcPos at{};
                std::string nm;
                if (Member* mem = memberAt(*d, line, col)) {
                    Type* r = unwrap(mem->object->type);
                    if (r && r->k == Type::Instance && r->cls && r->cls->module) {
                        auto mi = r->cls->methods.find(mem->name);
                        if (mi != r->cls->methods.end()) {
                            file = r->cls->module->path;
                            at = mi->second->namePos;
                            nm = mem->name;
                        }
                    } else if (r && r->k == Type::Module && r->module) {
                        auto g = r->module->globals.find(mem->name);
                        if (g != r->module->globals.end() && g->second->node) {
                            file = r->module->path;
                            Node* n = g->second->node;
                            at = n->kind == NK::FnDecl        ? static_cast<FnDecl*>(n)->namePos
                                 : n->kind == NK::ClassDecl   ? static_cast<ClassDecl*>(n)->namePos
                                 : n->kind == NK::InterfaceDecl ? static_cast<InterfaceDecl*>(n)->namePos
                                 : n->kind == NK::EnumDecl    ? static_cast<EnumDecl*>(n)->namePos
                                                              : SrcPos{};
                            nm = mem->name;
                        }
                    }
                }
                if (!file.empty() && at.line > 0) {
                    Json edits = Json::array();
                    Json e = Json::object();
                    Json r = rangeIn(file, SrcPos{at.line, at.col + (int)nm.size()}, 0);
                    r.set("end", r.get("start") ? *r.get("start") : Json());
                    e.set("range", r);
                    e.set("newText", "*");
                    edits.push(e);
                    auto open = std::find_if(docs.begin(), docs.end(),
                                             [&](auto& kv) { return canonicalPath(kv.second.path) == canonicalPath(file); });
                    out.push(action("Export '" + nm + "' (mark it with *)", diag,
                                    open != docs.end() ? open->second.uri : pathToUri(file), edits, true));
                }
            }
        }
        if (anyOld) {
            // every old spelling in the file at once (fixes that overlap wait for the next time)
            std::vector<const Diagnostic*> all;
            for (auto& f : d->fixable)
                if (f.deprecated) all.push_back(&f);
            std::string updated = d->text;
            int n = applyFixes(updated, all);
            if (n > 1) {
                Json edits = Json::array();
                auto [l2, c2] = lineColOf(*d, d->text.size());
                edits.push(textEdit(*d, 0, 0, l2, c2, updated));
                Json a = action("Update every old spelling in this file (" + std::to_string(n) + ")", Json::object(),
                                d->uri, edits);
                out.push(a);
            }
        }
        reply(id, out);
    }

    // The class declared at a name position (1-based), anywhere in the file.
    Node* nodeAtAnyName(Doc& d, int line, int col) {
        Node* found = nullptr;
        std::function<void(Node*)> walk = [&](Node* n) {
            if (!n || found) return;
            if (n->kind == NK::ClassDecl) {
                auto* c = static_cast<ClassDecl*>(n);
                if (c->namePos.line == line && c->namePos.col == col) found = c;
            }
            forEachChild(n, walk);
        };
        if (d.unit && d.unit->ast)
            for (auto& s : d.unit->ast->stmts) walk(s.get());
        return found;
    }

    // ---------------------------------------------------------- code lenses: run / debug a test

    void onCodeLens(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        Json out = Json::array();
        if (!d || !d->unit || !d->unit->ast) return reply(id, out);
        for (auto& st : d->unit->ast->stmts) {
            if (st->kind != NK::FnDecl) continue;
            auto* f = static_cast<FnDecl*>(st.get());
            if (std::find(f->attrs.begin(), f->attrs.end(), "test") == f->attrs.end()) continue;
            for (const char* which : {"run", "debug"}) {
                Json lens = Json::object();
                lens.set("range", range(*d, f->namePos, (int)f->name.size()));
                Json cmd = Json::object();
                cmd.set("title", std::string(which) == "run" ? "\u25B6 Run test" : "Debug test");
                cmd.set("command", std::string(which) == "run" ? "ixo.runTest" : "ixo.debugTest");
                Json args = Json::array();
                args.push(d->path);
                args.push(f->name);
                cmd.set("arguments", args);
                lens.set("command", cmd);
                out.push(lens);
            }
        }
        reply(id, out);
    }

    // ---------------------------------------------------------- inlay hints

    static SrcPos startOf(Node* n) {
        SrcPos best = n->pos;
        forEachChild(n, [&](Node* c) {
            if (!c) return;
            SrcPos p = startOf(c);
            if (p.line > 0 && (p.line < best.line || (p.line == best.line && p.col < best.col))) best = p;
        });
        return best;
    }

    // Parameter names of what a call calls, when we know it.
    std::vector<std::string> calleeParams(Call* c) {
        std::vector<std::string> names;
        Node* n = nullptr;
        bool method = false;
        if (c->callee->kind == NK::Ident) {
            Decl* dc = static_cast<Ident*>(c->callee.get())->decl;
            if (dc && dc->kind == DeclKind::Import && dc->importFrom && !dc->importFrom->isNative) {
                auto g = dc->importFrom->globals.find(dc->importName);
                if (g != dc->importFrom->globals.end()) dc = g->second;
            }
            if (dc && (dc->kind == DeclKind::Fn || dc->kind == DeclKind::Class)) n = dc->node;
        } else if (c->callee->kind == NK::Member) {
            auto* m = static_cast<Member*>(c->callee.get());
            Type* r = unwrap(m->object->type);
            if (r && r->k == Type::Instance && r->cls) {
                auto mi = r->cls->methods.find(m->name);
                if (mi != r->cls->methods.end()) n = mi->second;
                method = true;
            } else if (r && r->k == Type::EnumDef && r->en && r->en->node) {
                for (auto& v : r->en->node->variants)
                    if (v.name == m->name && !v.unit) n = v.cls.get();
            } else if (r && r->k == Type::Module && r->module) {
                if (Decl* ex = r->module->findExport(m->name)) n = ex->node;
            }
        }
        (void)method;
        if (!n) return names;
        if (n->kind == NK::FnDecl) {
            auto* f = static_cast<FnDecl*>(n);
            if (f->isLambda) return names;
            for (size_t i = f->selfFirst ? 1 : 0; i < f->params.size(); i++) names.push_back(f->params[i].name);
        } else if (n->kind == NK::ClassDecl) {
            auto* cd = static_cast<ClassDecl*>(n);
            if (cd->hasCtorParams)
                for (auto& p : cd->ctorParams) names.push_back(p.name);
            else if (cd->info && cd->info->init)
                for (auto& p : cd->info->init->params) names.push_back(p.name);
        }
        return names;
    }

    void onInlayHints(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        Json out = Json::array();
        if (!d || !d->unit || !d->unit->ast) return reply(id, out);
        int from = 0, to = 1 << 30;
        if (const Json* r = params.get("range")) {
            from = (int)r->at("start", "line")->num() + 1;
            to = (int)r->at("end", "line")->num() + 1;
        }
        auto hint = [&](SrcPos p, const std::string& label, int kind, bool padLeft, bool padRight) {
            if (p.line < from || p.line > to) return;
            Json h = Json::object();
            h.set("position", position(*d, p.line - 1, p.col - 1));
            h.set("label", label);
            h.set("kind", kind);
            if (padLeft) h.set("paddingLeft", true);
            if (padRight) h.set("paddingRight", true);
            out.push(h);
        };
        auto shown = [](Type* t) {
            return t && t->k != Type::Any && t->k != Type::TypeVar && t->k != Type::Fn && t->k != Type::Nil;
        };
        std::function<void(Node*)> walk = [&](Node* n) {
            if (!n) return;
            switch (n->kind) {
                case NK::Assign: {
                    auto* a = static_cast<Assign*>(n);
                    if (a->op == BinOp::None && !a->annot && a->target->kind == NK::Ident) {
                        auto* idn = static_cast<Ident*>(a->target.get());
                        Decl* dc = idn->decl;
                        if (dc && dc->kind == DeclKind::Var && dc->pos.line == idn->pos.line && dc->pos.col == idn->pos.col &&
                            !dc->annot && shown(dc->type))
                            hint(SrcPos{idn->pos.line, idn->pos.col + (int)idn->name.size()}, ": " + typeToString(dc->type), 1,
                                 false, false);
                    }
                    break;
                }
                case NK::For:
                    for (auto& v : static_cast<For*>(n)->vars)
                        if (v.decl && shown(v.decl->type) && v.name != "_")
                            hint(SrcPos{v.pos.line, v.pos.col + (int)v.name.size()}, ": " + typeToString(v.decl->type), 1,
                                 false, false);
                    break;
                case NK::Call: {
                    auto* c = static_cast<Call*>(n);
                    if (c->args.size() < 2) break;
                    std::vector<std::string> names = calleeParams(c);
                    for (size_t i = 0; i < c->args.size() && i < names.size(); i++) {
                        Expr* a = c->args[i].get();
                        if (!a || a->kind == NK::FnDecl || a->kind == NK::NamedArg) continue;
                        if (a->kind == NK::Ident && static_cast<Ident*>(a)->name == names[i]) continue;
                        if (a->kind == NK::Member && static_cast<Member*>(a)->name == names[i]) continue;
                        hint(startOf(a), names[i] + ":", 2, false, true);
                    }
                    break;
                }
                default: break;
            }
            forEachChild(n, walk);
        };
        for (auto& s : d->unit->ast->stmts) walk(s.get());
        reply(id, out);
    }

    // ---------------------------------------------------------- semantic tokens

    enum TokType { TNamespace, TClass, TEnum, TInterface, TEnumMember, TParameter, TVariable, TProperty, TFunction, TMethod };
    static constexpr const char* kTokenTypes[] = {"namespace", "class",    "enum",     "interface", "enumMember",
                                                  "parameter", "variable", "property", "function",  "method"};
    static constexpr const char* kTokenMods[] = {"declaration", "readonly", "defaultLibrary"};

    void onSemanticTokens(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        Json r = Json::object();
        Json data = Json::array();
        if (!d || !d->unit || !d->unit->ast) {
            r.set("data", data);
            return reply(id, r);
        }
        struct Tok {
            int line, col, len, type, mods;
        };
        std::vector<Tok> toks;
        VM& vm = d->prog->vm;
        auto add = [&](SrcPos p, size_t len, int type, int mods = 0) {
            if (p.line <= 0 || len == 0) return;
            toks.push_back({p.line - 1, p.col - 1, (int)len, type, mods});
        };
        auto kindType = [&](Decl* dc, int& mods) -> int {
            if (!dc) return -1;
            if (dc->kind == DeclKind::Import && dc->importFrom) {
                if (dc->importFrom->isNative) {
                    mods |= 4;
                    return TFunction;
                }
                auto g = dc->importFrom->globals.find(dc->importName);
                if (g == dc->importFrom->globals.end()) return -1;
                dc = g->second;
            }
            switch (dc->kind) {
                case DeclKind::Var: return TVariable;
                case DeclKind::Param: return TParameter;
                case DeclKind::Fn: return TFunction;
                case DeclKind::Class: return TClass;
                case DeclKind::Interface: return TInterface;
                case DeclKind::Enum: return TEnum;
                case DeclKind::Module: return TNamespace;
                case DeclKind::Builtin: {
                    mods |= 4;
                    int idx = dc->builtinIndex;
                    if (idx >= 0 && idx < (int)vm.builtins.size() && isObjType(vm.builtins[idx].value, OT::Namespace))
                        return TNamespace;
                    return TFunction;
                }
                default: return -1;
            }
        };
        std::function<void(Node*)> walk;
        std::function<void(Pattern*)> pat = [&](Pattern* p) {
            if (!p) return;
            if ((p->k == Pattern::Bind || p->k == Pattern::As) && isIdentifier(p->name) && p->name != "_")
                add(p->pos, p->name.size(), TVariable, 1);
            for (auto& s : p->subs) pat(s.get());
        };
        walk = [&](Node* n) {
            if (!n) return;
            switch (n->kind) {
                case NK::Ident: {
                    auto* x = static_cast<Ident*>(n);
                    if (x->name.empty() || x->name[0] == '_' || !isIdentifier(x->name)) break;
                    int mods = 0;
                    int t = kindType(x->decl, mods);
                    if (t >= 0) add(x->pos, x->name.size(), t, mods);
                    break;
                }
                case NK::Member: {
                    auto* m = static_cast<Member*>(n);
                    if (!isIdentifier(m->name)) break;
                    Type* r = unwrap(m->object->type);
                    int t = -1, mods = 0;
                    if (r && (r->k == Type::Instance || r->k == Type::Class) && r->cls) {
                        if (r->cls->fieldIndex.count(m->name)) t = TProperty;
                        else if (r->cls->methods.count(m->name)) t = TMethod;
                    } else if (r && r->k == Type::EnumDef) {
                        t = TEnumMember;
                    } else if (r && r->k == Type::Module && r->module) {
                        if (Decl* ex = r->module->findExport(m->name)) t = kindType(ex, mods);
                    } else if (r && (r->k == Type::Namespace || (r->k == Type::Module && r->module && r->module->isNative))) {
                        t = TFunction;
                        mods = 4;
                    } else if (r && r->k == Type::Interface) {
                        t = TMethod;
                    } else if (r) {
                        Tag tag = tagForType(r);
                        if (tag != Tag::COUNT && vm.methods[(int)tag].count(vm.sym(m->name))) {
                            t = TMethod;
                            mods = 4;
                        }
                    }
                    if (t >= 0) add(m->namePos, m->name.size(), t, mods);
                    break;
                }
                case NK::FnDecl: {
                    auto* f = static_cast<FnDecl*>(n);
                    if (!f->isLambda && isIdentifier(f->name)) add(f->namePos, f->name.size(), f->isMethod ? TMethod : TFunction, 1);
                    for (auto& p : f->params)
                        if (isIdentifier(p.name) && p.name != "self") add(p.pos, p.name.size(), TParameter, 1);
                    break;
                }
                case NK::ClassDecl: {
                    auto* c = static_cast<ClassDecl*>(n);
                    size_t dot = c->name.rfind('.');
                    if (dot != std::string::npos)
                        add(c->namePos, c->name.size() - dot - 1, TEnumMember, 1);
                    else
                        add(c->namePos, c->name.size(), TClass, 1);
                    for (auto& p : c->ctorParams) add(p.pos, p.name.size(), TProperty, 1);
                    for (auto& fd : c->fields) add(fd.pos, fd.name.size(), TProperty, 1);
                    break;
                }
                case NK::InterfaceDecl: {
                    auto* x = static_cast<InterfaceDecl*>(n);
                    add(x->namePos, x->name.size(), TInterface, 1);
                    break;
                }
                case NK::EnumDecl: {
                    auto* x = static_cast<EnumDecl*>(n);
                    add(x->namePos, x->name.size(), TEnum, 1);
                    break;
                }
                case NK::For:
                    for (auto& v : static_cast<For*>(n)->vars)
                        if (isIdentifier(v.name) && v.name != "_") add(v.pos, v.name.size(), TVariable, 1);
                    break;
                case NK::Match:
                    for (auto& arm : static_cast<Match*>(n)->arms) pat(arm.pat.get());
                    break;
                default: break;
            }
            forEachChild(n, walk);
        };
        for (auto& s : d->unit->ast->stmts) walk(s.get());
        std::sort(toks.begin(), toks.end(), [](const Tok& a, const Tok& b) {
            return a.line != b.line ? a.line < b.line : a.col < b.col;
        });
        int pl = 0, pc = 0;
        int lastLine = -1, lastEnd = -1;
        for (auto& t : toks) {
            if (t.line == lastLine && t.col < lastEnd) continue;  // overlapping
            if (t.line >= (int)d->lines.size()) continue;
            const std::string& lt = d->lines[t.line];
            int c16 = toUtf16(lt, t.col);
            int len16 = toUtf16(lt, t.col + t.len) - c16;
            data.push(t.line - pl);
            data.push(t.line == pl ? c16 - pc : c16);
            data.push(len16);
            data.push(t.type);
            data.push(t.mods);
            pl = t.line;
            pc = c16;
            lastLine = t.line;
            lastEnd = t.col + t.len;
        }
        r.set("data", data);
        reply(id, r);
    }

    void onFormatting(const Json& id, const Json& params) {
        Doc* d = docFor(params);
        Json edits = Json::array();
        if (!d) return reply(id, edits);
        std::string out, err;
        if (!formatSource(d->text, d->path, out, err) || out == d->text) return reply(id, edits);
        Json edit = Json::object();
        Json r = Json::object();
        Json start = Json::object();
        start.set("line", 0);
        start.set("character", 0);
        Json end = Json::object();
        end.set("line", (int)d->lines.size() + 1);
        end.set("character", 0);
        r.set("start", start);
        r.set("end", end);
        edit.set("range", r);
        edit.set("newText", out);
        edits.push(edit);
        reply(id, edits);
    }

    void handle(const Json& msg) {
        std::string method = msg.get("method") ? msg.get("method")->s : "";
        const Json* idp = msg.get("id");
        Json id = idp ? *idp : Json();
        Json empty = Json::object();
        const Json& params = msg.get("params") ? *msg.get("params") : empty;
        if (method == "initialize") {
            Json caps = Json::object();
            Json sync = Json::object();
            sync.set("openClose", true);
            sync.set("change", 1);
            Json save = Json::object();
            save.set("includeText", false);
            sync.set("save", save);
            caps.set("textDocumentSync", sync);
            caps.set("hoverProvider", true);
            caps.set("definitionProvider", true);
            Json comp = Json::object();
            Json triggers = Json::array();
            triggers.push(".");
            comp.set("triggerCharacters", triggers);
            caps.set("completionProvider", comp);
            caps.set("documentFormattingProvider", true);
            caps.set("documentSymbolProvider", true);
            caps.set("referencesProvider", true);
            Json ren = Json::object();
            ren.set("prepareProvider", true);
            caps.set("renameProvider", ren);
            Json sh = Json::object();
            Json shTriggers = Json::array();
            shTriggers.push("(");
            shTriggers.push(",");
            sh.set("triggerCharacters", shTriggers);
            caps.set("signatureHelpProvider", sh);
            Json ca = Json::object();
            Json kinds = Json::array();
            kinds.push("quickfix");
            ca.set("codeActionKinds", kinds);
            caps.set("codeActionProvider", ca);
            caps.set("inlayHintProvider", true);
            Json lens = Json::object();
            lens.set("resolveProvider", false);
            caps.set("codeLensProvider", lens);
            Json st = Json::object();
            Json legend = Json::object();
            Json types = Json::array();
            for (const char* t : kTokenTypes) types.push(t);
            Json mods = Json::array();
            for (const char* m : kTokenMods) mods.push(m);
            legend.set("tokenTypes", types);
            legend.set("tokenModifiers", mods);
            st.set("legend", legend);
            st.set("full", true);
            caps.set("semanticTokensProvider", st);
            // workspace folders, for workspace-wide references and rename
            if (const Json* folders = params.get("workspaceFolders"))
                for (auto& f : folders->arr)
                    if (f.get("uri")) roots.push_back(uriToPath(f.get("uri")->s));
            if (roots.empty())
                if (const Json* ru = params.get("rootUri"))
                    if (ru->k == Json::Str) roots.push_back(uriToPath(ru->s));
            Json result = Json::object();
            result.set("capabilities", caps);
            Json info = Json::object();
            info.set("name", "ixo");
            info.set("version", "0.6.0");
            result.set("serverInfo", info);
            return reply(id, result);
        }
        if (method == "initialized" || method == "$/cancelRequest" || method == "$/setTrace" ||
            (method.rfind("workspace/", 0) == 0 && !idp))
            return;
        if (method == "shutdown") {
            shutdownRequested = true;
            return reply(id, Json());
        }
        if (method == "exit") {
            exitNow = true;
            return;
        }
        if (method == "textDocument/didOpen") {
            const Json* td = params.get("textDocument");
            Doc& d = docs[td->get("uri")->s];
            d.uri = td->get("uri")->s;
            d.path = uriToPath(d.uri);
            d.text = td->get("text")->s;
            analyze(d);
            return;
        }
        if (method == "textDocument/didChange") {
            Doc* d = docFor(params);
            const Json* changes = params.get("contentChanges");
            if (!d || !changes || changes->arr.empty()) return;
            d->text = changes->arr.back().get("text")->s;
            analyze(*d);
            return;
        }
        if (method == "textDocument/didSave") {
            if (Doc* d = docFor(params)) analyze(*d);
            return;
        }
        if (method == "textDocument/didClose") {
            const Json* uri = params.at("textDocument", "uri");
            if (uri) {
                Json p = Json::object();
                p.set("uri", uri->s);
                p.set("diagnostics", Json::array());
                notify("textDocument/publishDiagnostics", p);
                docs.erase(uri->s);
            }
            return;
        }
        if (method == "textDocument/hover") return onHover(id, params);
        if (method == "textDocument/definition") return onDefinition(id, params);
        if (method == "textDocument/completion") return onCompletion(id, params);
        if (method == "textDocument/documentSymbol") return onSymbols(id, params);
        if (method == "textDocument/formatting") return onFormatting(id, params);
        if (method == "textDocument/references") return onReferences(id, params);
        if (method == "textDocument/prepareRename") return onPrepareRename(id, params);
        if (method == "textDocument/rename") return onRename(id, params);
        if (method == "textDocument/signatureHelp") return onSignatureHelp(id, params);
        if (method == "textDocument/codeAction") return onCodeAction(id, params);
        if (method == "textDocument/inlayHint") return onInlayHints(id, params);
        if (method == "textDocument/codeLens") return onCodeLens(id, params);
        if (method == "textDocument/semanticTokens/full") return onSemanticTokens(id, params);
        if (idp) replyError(id, -32601, "method not supported: " + method);
    }
};

}  // namespace

int runLanguageServer() {
    Server s;
    return s.run();
}

}  // namespace ixo
