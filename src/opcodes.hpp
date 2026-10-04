// Bytecode instruction set. Operands follow the opcode byte:
// u8 = 1 byte, u16 = 2 bytes little-endian, u32 = 4 bytes little-endian.
#pragma once

#include <cstdint>
#include <string>

namespace ixo {

enum Op : uint8_t {
    OP_CONST,              // u16 const
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_UNDEF,
    OP_POP,
    OP_DUP,
    OP_GET_LOCAL,          // u16 slot
    OP_GET_LOCAL_CHK,      // u16 slot, u16 nameConst   (errors if unassigned)
    OP_SET_LOCAL,          // u16 slot                  (pops)
    OP_GET_UPVAL,          // u16 index, u16 nameConst
    OP_SET_UPVAL,          // u16 index                 (pops)
    OP_GET_GLOBAL,         // u16 index
    OP_SET_GLOBAL,         // u16 index                 (pops)
    OP_GET_BUILTIN,        // u16 index
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW, OP_ROOT,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
    OP_NEG, OP_NOT, OP_BNOT,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_IS, OP_ISNOT, OP_IN, OP_NOTIN,
    OP_JUMP,               // u32 forward offset
    OP_JUMP_IF_FALSE,      // u32   pops the condition
    OP_JUMP_IF_FALSE_KEEP, // u32   (and) jump keeping the value, else pop
    OP_JUMP_IF_TRUE_KEEP,  // u32   (or)
    OP_JUMP_IF_NOT_ERROR,  // u32   peeks
    OP_JUMP_IF_DONE,       // u32   if top is Done: pop it and jump
    OP_LOOP,               // u32 backward offset
    OP_CALL,               // u8 argc
    OP_AUTOCALL,           // call the top value if it's a function taking no arguments
    OP_INVOKE,             // u16 sym, u8 argc, u8 fallbackKind, u16 fallbackIndex, u16 cache
    OP_GET_MEMBER,         // u16 sym, u8 fallbackKind, u16 fallbackIndex, u16 cache
    OP_GET_MEMBER_RAW,     // u16 sym, u8 fallbackKind, u16 fallbackIndex, u16 cache  (never auto-calls)
    OP_SET_MEMBER,         // u16 sym, u16 cache       [obj, value] -> []
    OP_GET_FIELD,          // u16 slot         [inst] -> [value]
    OP_SET_FIELD,          // u16 slot         [inst, value] -> []
    OP_GET_INDEX,          // [obj, index] -> [value]
    OP_SET_INDEX,          // [obj, index, value] -> []
    OP_CLOSURE,            // u16 fnConst, then per upvalue: u8 isLocal, u16 index
    OP_ENDSCOPE,           // u16 n   close upvalues and pop n values
    OP_ENDSCOPE_KEEP,      // u16 n   same, keeping the top value
    OP_RETURN,
    OP_LIST,               // u16 n
    OP_TUPLE,              // u16 n
    OP_MAP,                // u16 n (pairs)
    OP_SET,                // u16 n
    OP_RANGE,              // u8 flags: 1 inclusive, 2 has step
    OP_ITER_INIT,          // u8 mode (1 = key/value pairs)
    OP_ITER_NEXT,          // u16 slot of the iterator -> pushes value or Done
    OP_DESTRUCT,           // u8 n
    OP_CLASS,              // u16 templateConst
    OP_METHOD,             // u16 sym, u8 public   [class, closure] -> [class]
    OP_CTOR,               //                      [class, closure] -> [class]
    OP_TEST_TYPE,          // u8 Tag (0xFF = num, 0xFE = error, 0xFD = instance)
    OP_INSTANCE_OF,        // [value, class] -> [bool]
    OP_TEST_LEN,           // u8 cmp (0 exact, 1 at least), u8 kind (0 list, 1 tuple), u16 n
    OP_POS_FIELD,          // u16 i
    OP_ELEM,               // u16 i
    OP_SLICE_FROM,         // u16 start, u16 fromEnd
    OP_TRY,                // `?`: return the value if it's an error
    OP_FORK,               // u8 argc
    OP_SEND,               // [value, flow] -> [nil]
    OP_RECV,               // [flow] -> [value or nil]
    OP_RECV_ITER,          // [flow] -> [value or Done]
    OP_WITH_BEGIN,
    OP_WITH_END,
    OP_SLEEP,              // [ms] -> []
    OP_CHECK_TYPE,         // u16 typeConst, u16 whatConst  (peeks)
    OP_TODO,               // u16 msgConst
    OP_IMPORT,             // u16 module index (runs the module the first time)
    OP_IMPORT_DONE,        // u16 module index
    OP_EXPORT_GET,         // u16 nameConst   [module] -> [value]
    OP_FORMAT,             // u16 specConst (0xFFFF none)  [value] -> [string]
    OP_CONCAT,             // u16 n
    OP_SWAP,               // [a, b] -> [b, a]
    OP_ROT3,               // [a, b, c] -> [b, c, a]
    OP_DUP2,               // [a, b] -> [a, b, a, b]
    OP_JUMP_IF_SET,        // u16 slot, u32 offset: skip default-argument code if the slot has a value
    OP_PAR,                // u8 argc: start a parallel task
    OP_REGION_BEGIN,       // start a memory region (@arena, @frame, ...)
    OP_REGION_END,         // free what the region allocated and nothing references
    OP_GET_GLOBAL_OPT,     // u16 index: like GET_GLOBAL, but pushes Undef instead of failing
    OP_JUMP_IF_DEFINED,    // u32: jump (keeping the value) unless the top is Undef
    OP_INC_LOCAL,          // u16 slot, i8 delta: x += 1 / x++ on a local
    OP_MAKE_NS,            // u16 nameConst, u16 n, n x u16 memberNameConst: an enum's namespace
    OP_MATCH_VALUE,        // [value, pattern] -> [bool]: equal, or an instance of the class/interface/enum
    OP_CALL_NAMED,         // u8 argc, u8 nNamed, nNamed x u16 name: call with the last nNamed args by name
    OP_IDIV,               // a ~/ b: division rounded down (ints stay ints)
    OP_TEST_KEY,           // u16 key const (0xFFFF: none): [v] -> [v is a map (with the key)]
    OP_KEY,                // u16 key const: [map] -> [map[key]]
    OP_SELECT,             // u8 nFlows, u8 flags (1: timeout on top, 2: don't wait): -> (arm index, value)
    OP_JUMP_IF_NOT_CMP,    // u8 cmp (0 < 1 <= 2 > 3 >= 4 == 5 !=), u32: [a, b] -> [], jumps unless a cmp b
    OP_FOR_NEXT,           // u16 iterator slot, u32 exit: push the next item, or jump out when done
    OP_ITER_KEY,           // u16 iterator slot: push the position (or map key) of the item FOR_NEXT gave
    OP_INPLACE,            // u8 op: [a, b] -> [a op= b]: a's `+=` method if its class has one, else a op b
    OP_JUMP_IF_NOT_SEQ,    // u32: jump unless the top value is a list, range, tuple, str or set (kept)
    OP_LIST_APPEND,        // u16 slot: [v] -> []: adds v to the list in that local slot
    OP_RANGE_INIT,         // [start, end, step] -> the same as ints (checked like a range), for FOR_RANGE
    OP_FOR_RANGE,          // u16 slot (start, end, step), u8 flags (1 inclusive, 2 step is 1), u32 exit:
                           //   push the counter and advance it, or jump out when done
    OP_STR_APPEND,         // u16 slot, u8 form (0 `+=`, 1 `s = s + x`): [x] -> []: the local grows by x
                           //   (in place when it holds a string only this loop has seen)
    OP_STR_SEAL,           // u16 slot: the local's string may be seen elsewhere now (loop entry)
    OP_FSTR_BEGIN,         // -> [builder]: an empty string to build an f-string in
    OP_FSTR_ADD,           // u16 specConst (0xFFFF none): [builder, v] -> [builder + v as text]
    OP_FSTR_LIT,           // u16 const: [builder] -> [builder + the constant string]
    OP_STR_APPEND_G,       // u16 global, u8 form: like STR_APPEND for a module variable (in place
                           //   until something reads the variable: reading seals the string)
    OP_SEQ_INIT,           // [v] -> [list, 0] for a list (no iterator object), else [iterator, nil]
    OP_FOR_SEQ,            // u16 slot, u32 exit: like FOR_NEXT over SEQ_INIT's two slots
    OP_COUNT
};

// Fallback kinds for INVOKE / GET_MEMBER (UFCS: x.f(a) -> f(x, a)).
enum FallbackKind : uint8_t { FB_NONE, FB_LOCAL, FB_UPVAL, FB_GLOBAL, FB_BUILTIN };

const char* opName(uint8_t op);

}  // namespace ixo
