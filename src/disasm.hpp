#pragma once

#include <string>

#include "vm.hpp"

namespace ixo {

// Human-readable listing of a function's bytecode (and nested functions).
std::string disassemble(VM& vm, ObjFunction* fn);
// Bytes taken by the instruction at `at` (opcode and operands).
size_t instructionLength(ObjFunction* fn, size_t at);
const char* opName(uint8_t op);

}  // namespace ixo
