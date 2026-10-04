#include "disasm.hpp"

#include <cstdio>

namespace ixo {

const char* opName(uint8_t op) {
    static const char* names[] = {
        "CONST", "NIL", "TRUE", "FALSE", "UNDEF", "POP", "DUP", "GET_LOCAL", "GET_LOCAL_CHK", "SET_LOCAL",
        "GET_UPVAL", "SET_UPVAL", "GET_GLOBAL", "SET_GLOBAL", "GET_BUILTIN", "ADD", "SUB", "MUL", "DIV", "MOD",
        "POW", "ROOT", "BAND", "BOR", "BXOR", "SHL", "SHR", "NEG", "NOT", "BNOT", "EQ", "NE", "LT", "LE", "GT",
        "GE", "IS", "ISNOT", "IN", "NOTIN", "JUMP", "JUMP_IF_FALSE", "JUMP_IF_FALSE_KEEP", "JUMP_IF_TRUE_KEEP",
        "JUMP_IF_NOT_ERROR", "JUMP_IF_DONE", "LOOP", "CALL", "AUTOCALL", "INVOKE", "GET_MEMBER",
        "GET_MEMBER_RAW", "SET_MEMBER", "GET_FIELD", "SET_FIELD", "GET_INDEX", "SET_INDEX", "CLOSURE",
        "ENDSCOPE", "ENDSCOPE_KEEP", "RETURN", "LIST", "TUPLE", "MAP", "SET", "RANGE", "ITER_INIT", "ITER_NEXT",
        "DESTRUCT", "CLASS", "METHOD", "CTOR", "TEST_TYPE", "INSTANCE_OF", "TEST_LEN", "POS_FIELD", "ELEM",
        "SLICE_FROM", "TRY", "FORK", "SEND", "RECV", "RECV_ITER", "WITH_BEGIN", "WITH_END", "SLEEP",
        "CHECK_TYPE", "TODO", "IMPORT", "IMPORT_DONE", "EXPORT_GET", "FORMAT", "CONCAT", "SWAP", "ROT3", "DUP2",
        "JUMP_IF_SET", "PAR", "REGION_BEGIN", "REGION_END", "GET_GLOBAL_OPT", "JUMP_IF_DEFINED", "INC_LOCAL", "MAKE_NS", "MATCH_VALUE", "CALL_NAMED", "IDIV", "TEST_KEY", "KEY", "SELECT", "JUMP_IF_NOT_CMP", "FOR_NEXT", "ITER_KEY", "INPLACE", "JUMP_IF_NOT_SEQ", "LIST_APPEND",
        "RANGE_INIT", "FOR_RANGE", "STR_APPEND", "STR_SEAL", "FSTR_BEGIN", "FSTR_ADD", "FSTR_LIT",
        "STR_APPEND_G", "SEQ_INIT", "FOR_SEQ",
    };
    static_assert(sizeof(names) / sizeof(names[0]) == OP_COUNT, "opName table out of sync");
    return op < OP_COUNT ? names[op] : "???";
}

// Decodes the instruction at `at`: its text (when vm/row are given) and the
// offset of the next one. Nested functions (CLOSURE) go to `nested`.
static size_t decodeIns(VM* vm, ObjFunction* fn, size_t at, std::string* rowOut, std::vector<ObjFunction*>* nestedOut) {
    const auto& code = fn->chunk.code;
    const auto& consts = fn->chunk.consts;
    size_t i = at;
    std::string scratch;
    std::string& row = rowOut ? *rowOut : scratch;
    std::vector<ObjFunction*> noNested;
    std::vector<ObjFunction*>& nested = nestedOut ? *nestedOut : noNested;
    bool text = vm != nullptr && rowOut != nullptr;
    auto u8 = [&]() { return i < code.size() ? code[i++] : (uint8_t)0; };
    auto u16 = [&]() {
        uint16_t v = i + 1 < code.size() ? (uint16_t)(code[i] | (code[i + 1] << 8)) : 0;
        i += 2;
        return v;
    };
    auto u32 = [&]() {
        uint32_t v = i + 3 < code.size()
                         ? (uint32_t)(code[i] | (code[i + 1] << 8) | (code[i + 2] << 16) | ((uint32_t)code[i + 3] << 24))
                         : 0;
        i += 4;
        return v;
    };
    // without a VM only the lengths matter: names and constants aren't printed
    struct NoVm {
        std::string repr(Value) { return ""; }
        std::string toStr(Value) { return ""; }
        std::string symName(int) { return ""; }
    } novm;
    (void)text;
    uint8_t op = u8();
        switch (op) {
            case OP_CONST: {
                uint16_t k = u16();
                row += std::to_string(k) + " (" + (text ? vm->repr(consts[k]) : novm.repr(consts[k])) + ")";
                if (isObjType(consts[k], OT::Function)) nested.push_back(as<ObjFunction>(consts[k]));
                break;
            }
            case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_SET_UPVAL: case OP_GET_GLOBAL: case OP_SET_GLOBAL:
            case OP_GET_BUILTIN: case OP_GET_GLOBAL_OPT: case OP_ENDSCOPE: case OP_ENDSCOPE_KEEP: case OP_LIST: case OP_TUPLE:
            case OP_MAP: case OP_SET: case OP_ITER_NEXT: case OP_POS_FIELD: case OP_ELEM: case OP_GET_FIELD:
            case OP_SET_FIELD: case OP_IMPORT: case OP_IMPORT_DONE: case OP_CONCAT: case OP_ITER_KEY: case OP_LIST_APPEND:
                row += std::to_string(u16());
                break;
            case OP_INPLACE: row += opName(u8()); break;
            case OP_JUMP_IF_NOT_SEQ: {
                uint32_t off = u32();
                row += "-> " + std::to_string(i + off);
                break;
            }
            case OP_GET_LOCAL_CHK: case OP_GET_UPVAL: {
                uint16_t s = u16();
                uint16_t nm = u16();
                row += std::to_string(s) + " '" + (text ? vm->toStr(consts[nm]) : novm.toStr(consts[nm])) + "'";
                break;
            }
            case OP_SET_MEMBER: {
                row += (text ? vm->symName(u16()) : novm.symName(u16()));
                u16();
                break;
            }
            case OP_JUMP_IF_NOT_CMP: {
                static const char* cmps[] = {"<", "<=", ">", ">=", "==", "!="};
                int c = u8();
                row += std::string(c < 6 ? cmps[c] : "?") + " -> " + std::to_string(u32());
                break;
            }
            case OP_FOR_NEXT: case OP_FOR_SEQ: {
                row += "slot " + std::to_string(u16());
                row += " exit +" + std::to_string(u32());
                break;
            }
            case OP_STR_APPEND: {
                row += std::to_string(u16());
                row += u8() ? " (s = s + x)" : "";
                break;
            }
            case OP_STR_SEAL: row += std::to_string(u16()); break;
            case OP_STR_APPEND_G: {
                row += std::to_string(u16());
                row += u8() ? " (s = s + x)" : "";
                break;
            }
            case OP_FSTR_ADD: {
                uint16_t k = u16();
                if (k != 0xFFFF) row += (text ? vm->repr(consts[k]) : novm.repr(consts[k]));
                break;
            }
            case OP_FSTR_LIT: row += (text ? vm->repr(consts[u16()]) : novm.repr(consts[u16()])); break;
            case OP_FOR_RANGE: {
                row += "slot " + std::to_string(u16());
                uint8_t fl = u8();
                row += std::string((fl & 1) ? " ..=" : " ..") + ((fl & 2) ? "" : " by");
                row += " exit +" + std::to_string(u32());
                break;
            }
            case OP_EXPORT_GET: case OP_TODO: row += (text ? vm->repr(consts[u16()]) : novm.repr(consts[u16()])); break;
            case OP_FORMAT: {
                uint16_t k = u16();
                if (k != 0xFFFF) row += (text ? vm->repr(consts[k]) : novm.repr(consts[k]));
                break;
            }
            case OP_CHECK_TYPE: {
                uint16_t t = u16();
                uint16_t w = u16();
                row += (text ? vm->toStr(consts[t]) : novm.toStr(consts[t])) + " " + (text ? vm->repr(consts[w]) : novm.repr(consts[w]));
                break;
            }
            case OP_CLASS: row += (text ? vm->toStr(consts[u16()]) : novm.toStr(consts[u16()])); break;
            case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_FALSE_KEEP: case OP_JUMP_IF_TRUE_KEEP:
            case OP_JUMP_IF_NOT_ERROR: case OP_JUMP_IF_DONE: case OP_JUMP_IF_DEFINED: {
                uint32_t off = u32();
                row += "-> " + std::to_string(i + off);
                break;
            }
            case OP_SELECT: {
                int n = u8();
                int fl = u8();
                row += std::to_string(n) + " flows" + ((fl & 1) ? " +timeout" : "") + ((fl & 2) ? " +else" : "");
                break;
            }
            case OP_TEST_KEY:
            case OP_KEY: {
                uint16_t k = u16();
                row += k == 0xFFFF ? std::string("(map?)") : (text ? vm->repr(consts[k]) : novm.repr(consts[k]));
                break;
            }
            case OP_CALL_NAMED: {
                int argc = u8();
                int nn = u8();
                row += std::to_string(argc) + " (";
                for (int k = 0; k < nn; k++) row += (k ? ", " : "") + (text ? vm->toStr(consts[u16()]) : novm.toStr(consts[u16()]));
                row += ")";
                break;
            }
            case OP_MAKE_NS: {
                uint16_t nm = u16();
                uint16_t cnt = u16();
                row += (text ? vm->toStr(consts[nm]) : novm.toStr(consts[nm])) + " {";
                for (int k = 0; k < cnt; k++) row += (k ? ", " : "") + (text ? vm->toStr(consts[u16()]) : novm.toStr(consts[u16()]));
                row += "}";
                break;
            }
            case OP_INC_LOCAL: {
                uint16_t s = u16();
                int8_t d = (int8_t)u8();
                row += std::to_string(s) + (d >= 0 ? " +" : " ") + std::to_string(d);
                break;
            }
            case OP_JUMP_IF_SET: {
                uint16_t s = u16();
                uint32_t off = u32();
                row += std::to_string(s) + " -> " + std::to_string(i + off);
                break;
            }
            case OP_LOOP: {
                uint32_t off = u32();
                row += "-> " + std::to_string(i - off);
                break;
            }
            case OP_CALL: case OP_FORK: case OP_PAR: case OP_DESTRUCT: case OP_ITER_INIT: case OP_RANGE: case OP_TEST_TYPE:
                row += std::to_string(u8());
                break;
            case OP_INVOKE: {
                uint16_t s = u16();
                uint8_t argc = u8();
                uint8_t fk = u8();
                uint16_t fi = u16();
                u16();
                row += (text ? vm->symName(s) : novm.symName(s)) + " argc=" + std::to_string(argc) + " fb=" + std::to_string(fk) + ":" + std::to_string(fi);
                break;
            }
            case OP_GET_MEMBER: case OP_GET_MEMBER_RAW: {
                uint16_t s = u16();
                uint8_t fk = u8();
                uint16_t fi = u16();
                u16();
                row += (text ? vm->symName(s) : novm.symName(s)) + " fb=" + std::to_string(fk) + ":" + std::to_string(fi);
                break;
            }
            case OP_METHOD: {
                uint16_t s = u16();
                uint8_t pub = u8();
                row += (text ? vm->symName(s) : novm.symName(s)) + (pub ? " public" : " private");
                break;
            }
            case OP_TEST_LEN: {
                uint8_t cmp = u8();
                uint8_t kind = u8();
                uint16_t k = u16();
                row += std::string(kind ? "tuple " : "list ") + (cmp ? ">= " : "== ") + std::to_string(k);
                break;
            }
            case OP_SLICE_FROM: {
                uint16_t s = u16();
                uint16_t e = u16();
                row += std::to_string(s) + " .. -" + std::to_string(e);
                break;
            }
            case OP_CLOSURE: {
                uint16_t k = u16();
                auto* f = as<ObjFunction>(consts[k]);
                nested.push_back(f);
                row += f->name;
                for (int u = 0; u < f->upvalueCount; u++) {
                    uint8_t isLocal = u8();
                    uint16_t idx = u16();
                    row += std::string(isLocal ? " local:" : " up:") + std::to_string(idx);
                }
                break;
            }
            default: break;
        }
    return i;
}

size_t instructionLength(ObjFunction* fn, size_t at) { return decodeIns(nullptr, fn, at, nullptr, nullptr) - at; }

static void disasmFn(VM& vm, ObjFunction* fn, std::string& out, std::vector<ObjFunction*>& nested) {
    const auto& code = fn->chunk.code;
    char buf[256];
    snprintf(buf, sizeof buf, "== %s (arity %d, upvalues %d) ==\n", fn->name.c_str(), fn->arity, fn->upvalueCount);
    out += buf;
    size_t i = 0;
    int lastLine = -1;
    while (i < code.size()) {
        size_t at = i;
        int line = fn->chunk.lineAt(at);
        uint8_t op = code[at];
        std::string lineStr = line == lastLine ? "   |" : (snprintf(buf, sizeof buf, "%4d", line), std::string(buf));
        lastLine = line;
        snprintf(buf, sizeof buf, "%05zu %s  %-18s", at, lineStr.c_str(), opName(op));
        std::string row = buf;
        i = decodeIns(&vm, fn, at, &row, &nested);
        out += row + "\n";
    }
}

std::string disassemble(VM& vm, ObjFunction* fn) {
    std::string out;
    std::vector<ObjFunction*> todo{fn};
    for (size_t i = 0; i < todo.size(); i++) {
        std::vector<ObjFunction*> nested;
        disasmFn(vm, todo[i], out, nested);
        out += "\n";
        for (auto* n : nested) todo.push_back(n);
    }
    return out;
}

}  // namespace ixo
