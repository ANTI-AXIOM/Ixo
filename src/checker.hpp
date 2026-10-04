// Gradual type checker.
//
// Types come from annotations, literals, builtin signatures and inference
// (variables get the join of everything assigned to them; functions the join
// of what they return). Anything unknown is `any`, and `any` is compatible
// with everything, so unannotated code is never rejected. Only definite
// mistakes are reported: wrong argument counts, members that don't exist,
// `str + int`, assigning a str to an `int` variable, and so on.
#pragma once

#include <string>
#include <vector>

#include "module.hpp"

namespace ixo {

class Program;

struct Type {
    enum K : uint8_t {
        Any, Nil, Bool, Int, Float, Num, Str, List, Map, Set, Tuple, Fn, Instance, Class, Range, Flow,
        Task, Error, Result, Optional, Vec, Mat, Quat, Rng, Time, Module, Namespace, Iter, TypeVar,
        Interface,  // a value of an interface type
        Enum,       // a value of an enum (any of its variants)
        EnumDef,    // the enum itself: Color in Color.Red
    } k = Any;
    std::vector<Type*> args;  // element / key+value / tuple items / fn params / inner
    Type* ret = nullptr;      // fn return
    ClassInfo* cls = nullptr; // Instance / Class
    FnDecl* fn = nullptr;     // Fn: its declaration, if known
    int minArgs = 0, maxArgs = -1;  // Fn arity; maxArgs -1 = unknown / variadic
    bool arityKnown = false;
    std::string name;         // TypeVar / Namespace / Module name, builtin fn name
    ModuleUnit* module = nullptr;
    InterfaceInfo* iface = nullptr;
    EnumInfo* en = nullptr;
    bool declared = false;  // Optional written in an annotation (`str?`): using it unchecked warns
};

std::string typeToString(const Type* t);

class Checker {
public:
    Checker(Program& prog, ModuleUnit& mod, Diagnostics& diags);
    ~Checker();
    void check(Block* program);

private:
    struct Impl;
    Impl* impl;
};

}  // namespace ixo
