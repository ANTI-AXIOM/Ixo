// Debugger hook for the VM. When `VM::debugger` is set, the VM calls check()
// before every instruction and onPanic() when the program panics; the
// debugger may block there (the program is then "stopped") while another
// thread inspects the VM. `ixo dap` implements it with the Debug Adapter
// Protocol (dap.cpp).
#pragma once

#include <string>

namespace ixo {

class VM;
struct Fiber;

class Debugger {
public:
    virtual ~Debugger() = default;
    virtual void check(VM& vm, Fiber* f) = 0;
    virtual void onPanic(VM& vm, Fiber* f, const std::string& msg) = 0;
    virtual bool allowsJit() const { return false; }  // machine code skips check()
};

int runDebugAdapter();  // `ixo dap`: a debug adapter speaking DAP over stdio

}  // namespace ixo
