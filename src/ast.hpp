// Abstract syntax tree. Nodes are plain structs tagged with a kind; passes
// switch on `kind` and static_cast. The resolver fills in the Decl pointers
// that the checker and compiler share.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.hpp"

namespace ixo {

struct Decl;
struct FuncInfo;
struct ClassInfo;
struct InterfaceInfo;
struct EnumInfo;
struct Type;
struct ModuleUnit;
struct FnDecl;
struct Block;

// ---------------------------------------------------------------- types

struct TypeExpr {
    enum K { Named, Tuple, Fn, Optional, Result, Ref } k = Named;
    SrcPos pos;
    std::string name;  // Named: "int", "list", "Point", ...
    std::vector<std::unique_ptr<TypeExpr>> args;  // generic args / tuple elems / fn params
    std::unique_ptr<TypeExpr> ret;  // Fn return, Result error type
    bool mut = false;               // Ref: &mut
    bool typeVar = false;           // a type parameter: T, or Key in class Cache[Key, Value]
    std::string str() const;
};
using TypeP = std::unique_ptr<TypeExpr>;

// ---------------------------------------------------------------- nodes

enum class NK : uint8_t {
    // expressions
    IntLit, FloatLit, StrLit, BoolLit, NilLit, FStr, Ident, Unary, Binary, Range, Call,
    Member, Index, ListLit, TupleLit, MapLit, SetLit, Lambda, If, Match, Catch, Try, Fork,
    // statements
    ExprStmt, Assign, Destructure, VarDecl, IncDec, Block, While, For, ForRecv, Return,
    Break, Continue, Retry, FnDecl, ClassDecl, Use, With, Region, Recv, Todo, Defer, InterfaceDecl,
    EnumDecl,
    // expressions added later
    NamedArg,
    // statements added later
    Select,
};

struct Node {
    NK kind;
    SrcPos pos;
    Node(NK k, SrcPos p) : kind(k), pos(p) {}
    virtual ~Node() = default;
};
struct Expr : Node {
    using Node::Node;
    Type* type = nullptr;  // filled by the checker (owned by the checker's arena)
};
struct Stmt : Node {
    using Node::Node;
};
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

enum class BinOp : uint8_t {
    None, Add, Sub, Mul, Div, Mod, Pow, Root, Eq, Ne, Lt, Le, Gt, Ge, Is, IsNot, In, NotIn,
    And, Or, BitAnd, BitOr, BitXor, Shl, Shr, Send, IntDiv,
};
enum class UnOp : uint8_t { Neg, Not, BitNot };

const char* binOpText(BinOp op);

struct IntLit : Expr {
    int64_t value;
    IntLit(SrcPos p, int64_t v) : Expr(NK::IntLit, p), value(v) {}
};
struct FloatLit : Expr {
    double value;
    FloatLit(SrcPos p, double v) : Expr(NK::FloatLit, p), value(v) {}
};
struct StrLit : Expr {
    std::string value;
    bool docString = false;  // a """...""" used as a statement
    StrLit(SrcPos p, std::string v) : Expr(NK::StrLit, p), value(std::move(v)) {}
};
struct BoolLit : Expr {
    bool value;
    BoolLit(SrcPos p, bool v) : Expr(NK::BoolLit, p), value(v) {}
};
struct NilLit : Expr {
    explicit NilLit(SrcPos p) : Expr(NK::NilLit, p) {}
};
struct FStr : Expr {
    struct Part {
        std::string text;  // literal text when expr is null
        ExprP expr;
        std::string spec;  // format spec after ':' e.g. ".2f"
    };
    std::vector<Part> parts;
    explicit FStr(SrcPos p) : Expr(NK::FStr, p) {}
};
struct Ident : Expr {
    std::string name;
    Decl* decl = nullptr;
    bool statementCall = false;  // standalone statement: call it if it's a 0-arg function
    bool callee = false;  // the function of a call: f(x), fork f
    bool ref = false;     // &tick: the function itself, never called
    size_t start = 0;     // byte offset in the source (fixes)
    // In a method, a field or method used by its bare name (hp, alive): the
    // resolver puts `self.name` here, and the name means that member.
    ExprP selfMember;
    Ident(SrcPos p, std::string n) : Expr(NK::Ident, p), name(std::move(n)) {}
    bool isPlaceholder() const { return !name.empty() && name[0] == '_'; }
};
struct Unary : Expr {
    UnOp op;
    ExprP operand;
    Unary(SrcPos p, UnOp o, ExprP e) : Expr(NK::Unary, p), op(o), operand(std::move(e)) {}
};
struct Binary : Expr {
    BinOp op;
    ExprP left, right;
    Binary(SrcPos p, BinOp o, ExprP l, ExprP r)
        : Expr(NK::Binary, p), op(o), left(std::move(l)), right(std::move(r)) {}
};
struct Range : Expr {
    ExprP start, end, step;
    bool inclusive;
    Range(SrcPos p, ExprP s, ExprP e, bool inc)
        : Expr(NK::Range, p), start(std::move(s)), end(std::move(e)), inclusive(inc) {}
};
struct Call : Expr {
    ExprP callee;
    std::vector<ExprP> args;
    std::vector<TypeP> typeArgs;  // flow[int](10)
    size_t lparen = 0, rparen = 0;  // byte offsets of the parentheses (fixes)
    Call(SrcPos p, ExprP c) : Expr(NK::Call, p), callee(std::move(c)) {}
};
// obj.name   or   obj.name(args)
struct Member : Expr {
    ExprP object;
    std::string name;
    SrcPos namePos;
    bool isCall = false;
    std::vector<ExprP> args;
    Decl* ufcs = nullptr;  // free function `name` visible here (UFCS fallback)
    bool ref = false;      // &obj.method: the bound method, never called
    size_t nameEnd = 0;    // byte offset just after the name (fixes)
    Member(SrcPos p, ExprP o, std::string n, SrcPos np)
        : Expr(NK::Member, p), object(std::move(o)), name(std::move(n)), namePos(np) {}
};
// f(x, verbose: true): an argument passed by parameter name
struct NamedArg : Expr {
    std::string name;
    ExprP value;
    NamedArg(SrcPos p, std::string n, ExprP v) : Expr(NK::NamedArg, p), name(std::move(n)), value(std::move(v)) {}
};
struct Index : Expr {
    ExprP object, index;
    Index(SrcPos p, ExprP o, ExprP i) : Expr(NK::Index, p), object(std::move(o)), index(std::move(i)) {}
};
struct ListLit : Expr {
    std::vector<ExprP> elems;
    explicit ListLit(SrcPos p) : Expr(NK::ListLit, p) {}
};
struct TupleLit : Expr {
    std::vector<ExprP> elems;
    explicit TupleLit(SrcPos p) : Expr(NK::TupleLit, p) {}
};
struct MapLit : Expr {
    std::vector<ExprP> keys, values;
    explicit MapLit(SrcPos p) : Expr(NK::MapLit, p) {}
};
struct SetLit : Expr {
    std::vector<ExprP> elems;
    explicit SetLit(SrcPos p) : Expr(NK::SetLit, p) {}
};
struct Lambda : Expr {
    std::unique_ptr<FnDecl> fn;
    Lambda(SrcPos p, std::unique_ptr<FnDecl> f);
    ~Lambda() override;
};
struct If : Expr {
    ExprP cond;
    std::unique_ptr<Block> then;
    std::unique_ptr<Block> otherwise;  // may hold a single nested If for elif
    bool isElif = false;
    If(SrcPos p);
    ~If() override;
};

struct Pattern {
    enum K { Wildcard, Bind, Literal, RangeP, TypeP_, ClassP, TupleP, ListP, Alt, As, Value, MapP } k;
    SrcPos pos;
    std::string name;     // Bind / As / TypeP_ name / ClassP name
    Decl* decl = nullptr; // Bind / As binding, ClassP class decl (via classRef)
    ExprP value, value2;  // Literal/Value: value; RangeP: lo, hi; ClassP: class reference
    bool inclusive = false;
    std::vector<std::unique_ptr<Pattern>> subs;
    std::vector<ExprP> keys;   // MapP: the literal keys (parallel to subs)
    int restIndex = -1;        // ListP: position of `..rest`
    std::string restName;      // may be empty for `..`
    Decl* restDecl = nullptr;
    SrcPos restPos;
    Pattern(K k, SrcPos p) : k(k), pos(p) {}
};
using PatternP = std::unique_ptr<Pattern>;

struct MatchArm {
    PatternP pat;
    ExprP guard;
    std::unique_ptr<Block> body;
};
struct Match : Expr {
    ExprP subject;
    std::vector<MatchArm> arms;
    explicit Match(SrcPos p) : Expr(NK::Match, p) {}
    ~Match() override;
};
struct Catch : Expr {
    ExprP expr;
    std::string errName;  // may be "_" or empty
    SrcPos errPos;
    Decl* errDecl = nullptr;
    std::unique_ptr<Block> handler;
    bool hasRetry = false;
    Catch(SrcPos p);
    ~Catch() override;
};
struct Try : Expr {
    ExprP expr;
    Try(SrcPos p, ExprP e) : Expr(NK::Try, p), expr(std::move(e)) {}
};
struct Fork : Expr {
    ExprP call;  // Call, Member (method call), Ident or an immediately-called Lambda
    bool parallel = false;  // `par`: runs on another thread with its own heap
    Fork(SrcPos p, ExprP c) : Expr(NK::Fork, p), call(std::move(c)) {}
};

// ---------------------------------------------------------------- statements

struct ExprStmt : Stmt {
    ExprP expr;
    ExprStmt(SrcPos p, ExprP e) : Stmt(NK::ExprStmt, p), expr(std::move(e)) {}
};
struct Assign : Stmt {
    ExprP target;      // Ident, Member or Index
    ExprP value;
    BinOp op = BinOp::None;  // compound assignment operator
    TypeP annot;       // x: int = 5
    bool exported = false;  // limit* = 10 (module level)
    Assign(SrcPos p, ExprP t, ExprP v) : Stmt(NK::Assign, p), target(std::move(t)), value(std::move(v)) {}
};
struct Destructure : Stmt {
    std::vector<ExprP> targets;
    ExprP value;
    explicit Destructure(SrcPos p) : Stmt(NK::Destructure, p) {}
};
struct VarDecl : Stmt {
    std::string name;
    Decl* decl = nullptr;
    TypeP annot;
    ExprP init;
    VarDecl(SrcPos p, std::string n) : Stmt(NK::VarDecl, p), name(std::move(n)) {}
};
// select: wait for the first of several flows (or a timeout)
struct SelectArm {
    enum K { Recv, After, Else } kind = Recv;
    std::string name;  // Recv: the variable ("_" or empty: none)
    SrcPos pos, namePos;
    std::unique_ptr<Expr> expr;  // Recv: the flow; After: milliseconds
    std::unique_ptr<struct Block> body;
    Decl* decl = nullptr;
};
struct Select : Stmt {
    std::vector<SelectArm> arms;
    explicit Select(SrcPos p) : Stmt(NK::Select, p) {}
    ~Select() override;
};
struct IncDec : Stmt {
    ExprP target;
    bool inc;
    IncDec(SrcPos p, ExprP t, bool i) : Stmt(NK::IncDec, p), target(std::move(t)), inc(i) {}
};
struct Block : Stmt {
    std::vector<StmtP> stmts;
    explicit Block(SrcPos p) : Stmt(NK::Block, p) {}
};
struct While : Stmt {
    ExprP cond;
    std::unique_ptr<Block> body;
    bool jitHint = false;  // $jit while ...
    explicit While(SrcPos p) : Stmt(NK::While, p) {}
};
struct LoopVar {
    std::string name;
    SrcPos pos;
    Decl* decl = nullptr;
};
struct For : Stmt {
    std::vector<LoopVar> vars;
    ExprP iter;
    ExprP step;  // for i in 0..10 by 2
    // for (a, b) in pairs: unpack each item. Without parentheses, two names are
    // the position (or key) and the item: for i, x in list / for k, v in map.
    bool destructure = false;
    bool unpackItem = false;
    bool jitHint = false;     // $jit for ...: machine code from the first iteration  // for i, (a, b) in pairs: the position, then the item unpacked
    size_t fixStart = 0, fixEnd = 0;  // byte range of `a, b` (fixes)
    std::unique_ptr<Block> body;
    explicit For(SrcPos p) : Stmt(NK::For, p) {}
};
struct ForRecv : Stmt {
    std::vector<LoopVar> vars;
    std::vector<ExprP> chans;
    std::unique_ptr<Block> body;
    explicit ForRecv(SrcPos p) : Stmt(NK::ForRecv, p) {}
};
struct Return : Stmt {
    ExprP value;
    explicit Return(SrcPos p) : Stmt(NK::Return, p) {}
};
struct Break : Stmt {
    explicit Break(SrcPos p) : Stmt(NK::Break, p) {}
};
struct Continue : Stmt {
    explicit Continue(SrcPos p) : Stmt(NK::Continue, p) {}
};
struct Retry : Stmt {
    ExprP count, delay;
    explicit Retry(SrcPos p) : Stmt(NK::Retry, p) {}
};

struct Param {
    std::string name;
    SrcPos pos;
    TypeP type;
    ExprP def;
    bool mut = false;
    Decl* decl = nullptr;
};

struct FnDecl : Stmt {
    std::string name;
    SrcPos namePos;
    std::vector<Param> params;
    TypeP ret;
    std::unique_ptr<Block> body;
    bool exported = false;
    bool isFork = false;         // fork fn: every call starts a task
    bool isPar = false;          // par fn: every call starts a parallel task
    bool isLambda = false;
    bool isMethod = false;       // defined in a class body (implicit self)
    bool lenientArity = false;   // placeholder lambdas ignore extra arguments
    bool selfFirst = false;      // prelude extension method: first param is `self`
    bool hasBody = true;
    bool hasParamList = false;   // written with parentheses: fn f() vs fn tick
    std::vector<std::string> typeParams;  // fn first[T](xs: list[T])
    std::vector<std::string> attrs;  // @test, $jit, ...
    std::string doc;                 // docstring (first statement of the body)
    FuncInfo* info = nullptr;
    Decl* decl = nullptr;
    ClassInfo* cls = nullptr;
    SrcPos endPos;
    explicit FnDecl(SrcPos p) : Stmt(NK::FnDecl, p) {}
};

struct FieldDecl {
    std::string name;
    SrcPos pos;
    TypeP type;
    ExprP def;
};

struct ClassDecl : Stmt {
    std::string name;
    SrcPos namePos;
    std::vector<Param> ctorParams;
    bool hasCtorParams = false;
    std::vector<FieldDecl> fields;
    std::vector<std::unique_ptr<FnDecl>> methods;
    std::vector<std::pair<std::string, SrcPos>> interfaces;  // class Circle(r) is Shape
    std::vector<std::string> typeParams;                      // class Box[T]
    std::vector<FnDecl*> shared;  // methods it gets from interfaces (defaults) or its enum
    bool exported = false;
    std::string doc;
    ClassInfo* info = nullptr;
    Decl* decl = nullptr;
    explicit ClassDecl(SrcPos p) : Stmt(NK::ClassDecl, p) {}
};

struct InterfaceDecl : Stmt {
    std::string name;
    SrcPos namePos;
    bool exported = false;
    std::vector<std::unique_ptr<FnDecl>> methods;  // without a body: required
    std::vector<std::pair<std::string, SrcPos>> bases;  // interface Solid is Shape
    std::string doc;
    InterfaceInfo* info = nullptr;
    Decl* decl = nullptr;
    explicit InterfaceDecl(SrcPos p) : Stmt(NK::InterfaceDecl, p) {}
};

struct EnumVariant {
    std::string name;
    SrcPos pos;
    bool unit = true;                 // no data: a single value
    std::unique_ptr<ClassDecl> cls;   // each variant is a small class
};

struct EnumDecl : Stmt {
    std::string name;
    SrcPos namePos;
    bool exported = false;
    std::vector<EnumVariant> variants;
    std::vector<std::unique_ptr<FnDecl>> methods;  // shared by every variant
    std::string doc;
    EnumInfo* info = nullptr;
    Decl* decl = nullptr;
    explicit EnumDecl(SrcPos p) : Stmt(NK::EnumDecl, p) {}
};

struct UseItem {
    std::string name, alias;
    SrcPos pos;
    Decl* decl = nullptr;
};
struct Use : Stmt {
    std::vector<std::string> path;  // a.b.c
    std::string alias;              // use a as b
    std::vector<UseItem> items;     // in a use x, y  (empty => everything exported)
    bool selective = false;
    ModuleUnit* module = nullptr;
    Decl* moduleDecl = nullptr;     // binding for the module name itself
    std::vector<std::pair<Decl*, std::string>> imported;  // local decl <- exported name
    explicit Use(SrcPos p) : Stmt(NK::Use, p) {}
    std::string dotted() const {
        std::string s;
        for (size_t i = 0; i < path.size(); i++) s += (i ? "." : "") + path[i];
        return s;
    }
};
struct With : Stmt {
    std::unique_ptr<Block> body;
    explicit With(SrcPos p) : Stmt(NK::With, p) {}
};
struct Region : Stmt {
    std::string kind;  // arena, frame, temp, ...
    std::string name;  // @cache assets
    SrcPos namePos;
    ExprP init;        // @dual buffers = make_buffer()  (evaluated for each side)
    std::unique_ptr<Block> body;
    Decl* nameDecl = nullptr;    // the block-local variable `name`
    std::vector<Decl*> scopedVars;  // variables first assigned in an arena block live only there
    Decl* storeDecl = nullptr;   // hidden module variable keeping the object between runs
    explicit Region(SrcPos p) : Stmt(NK::Region, p) {}
    bool isArena() const { return kind != "cache" && kind != "dual"; }
};
struct Defer : Stmt {
    std::unique_ptr<Block> body;
    explicit Defer(SrcPos p) : Stmt(NK::Defer, p) {}
};
struct Recv : Stmt {
    ExprP target;  // Ident
    ExprP chan;
    explicit Recv(SrcPos p) : Stmt(NK::Recv, p) {}
};
struct Todo : Stmt {
    std::string message;
    explicit Todo(SrcPos p) : Stmt(NK::Todo, p) {}
};

inline Lambda::Lambda(SrcPos p, std::unique_ptr<FnDecl> f) : Expr(NK::Lambda, p), fn(std::move(f)) {}
inline Lambda::~Lambda() = default;
inline If::If(SrcPos p) : Expr(NK::If, p) {}
inline If::~If() = default;
inline Match::~Match() = default;
inline Catch::Catch(SrcPos p) : Expr(NK::Catch, p) {}
inline Catch::~Catch() = default;

// ---------------------------------------------------------------- resolver data

enum class DeclKind : uint8_t {
    Var,      // assigned variable
    Param,
    Fn,
    Class,
    Import,   // imported from another module
    Module,   // a module bound by `use`
    Interface,
    Enum,
    Builtin,
    Self,
    Hidden,
};

struct Decl {
    std::string name;
    DeclKind kind = DeclKind::Var;
    SrcPos pos;
    std::string file;
    FuncInfo* owner = nullptr;  // function whose frame stores it; null for globals
    bool isGlobal = false;
    int globalIndex = -1;
    bool blockScoped = false;  // loop vars, `var`, pattern bindings
    bool captured = false;
    int builtinIndex = -1;
    TypeExpr* annot = nullptr;
    Node* node = nullptr;      // FnDecl / ClassDecl that defines it
    ModuleUnit* module = nullptr;       // module that owns this global
    ModuleUnit* importFrom = nullptr;   // Import: source module
    std::string importName;             // Import: name in the source module
    int assignCount = 0;
    Type* type = nullptr;               // checker's inferred/declared type
    bool typeFixed = false;             // declared with an annotation
    bool mayBeUnset = false;            // block-scoped but assigned later (arena variables)
    bool exported = false;              // limit* = 10
};

struct FuncInfo {
    FnDecl* node = nullptr;       // null for a module's top-level code
    FuncInfo* parent = nullptr;
    ModuleUnit* module = nullptr;
    ClassInfo* cls = nullptr;     // method's class
    std::vector<Decl*> params;
    std::vector<Decl*> hoisted;   // function-scoped locals (assigned with `=`)
    Decl* selfDecl = nullptr;
    bool isModule = false;
};

struct ClassInfo {
    std::string name;
    ClassDecl* node = nullptr;
    ModuleUnit* module = nullptr;
    std::vector<std::string> fields;
    std::unordered_map<std::string, int> fieldIndex;
    std::vector<TypeExpr*> fieldTypes;    // parallel to fields (may be null)
    std::vector<int> positional;          // field indices for Point(x, y) patterns
    std::unordered_map<std::string, FnDecl*> methods;
    FnDecl* init = nullptr;
    FuncInfo* ctorInfo = nullptr;  // synthetic constructor (field defaults + init call)
    std::vector<InterfaceInfo*> interfaces;
    EnumInfo* enumOf = nullptr;    // enum variants
    std::vector<std::string> typeParams;  // generic classes: class Box[T]
    Decl* decl = nullptr;
    int addField(const std::string& n, TypeExpr* t) {
        auto it = fieldIndex.find(n);
        if (it != fieldIndex.end()) {
            if (t && !fieldTypes[it->second]) fieldTypes[it->second] = t;
            return it->second;
        }
        int idx = (int)fields.size();
        fields.push_back(n);
        fieldTypes.push_back(t);
        fieldIndex[n] = idx;
        return idx;
    }
};

struct InterfaceInfo {
    std::string name;
    ModuleUnit* module = nullptr;
    InterfaceDecl* node = nullptr;
    std::unordered_map<std::string, FnDecl*> methods;
    std::vector<std::string> required;
};

struct EnumInfo {
    std::string name;
    ModuleUnit* module = nullptr;
    EnumDecl* node = nullptr;
    std::vector<std::string> variantNames;
    std::vector<ClassInfo*> variants;
};

// Calls f on every direct child node (expressions and statements).
void forEachChild(Node* n, const std::function<void(Node*)>& f);

// Collects placeholder identifiers (_ , _a, _name) inside a node, without
// descending into nested lambdas.
void collectPlaceholders(Node* n, std::vector<std::string>& out);

bool isBuiltinTypeName(const std::string& name);

// Assignment targets that are bare field names (hp = 3 in a method) are
// swapped for their `self.hp` member while a pass looks at them, then put back.
struct SelfTargetSwap {
    std::vector<std::pair<ExprP*, ExprP>> saved;
    void in(ExprP& t) {
        if (!t || t->kind != NK::Ident || !static_cast<Ident*>(t.get())->selfMember) return;
        ExprP member = std::move(static_cast<Ident*>(t.get())->selfMember);
        saved.push_back({&t, std::move(t)});
        t = std::move(member);
    }
    ~SelfTargetSwap() {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            auto* id = static_cast<Ident*>(it->second.get());
            id->selfMember = std::move(*it->first);
            *it->first = std::move(it->second);
        }
    }
};

}  // namespace ixo
