# Evaluation: an LLVM backend for Ixo

Should Ixo compile programs with LLVM instead of its own JIT? This page measures what
that would gain and cost, and recommends a path.

## What would change

Ixo's compiler, VM, runtime and JIT are written in C++. Ixo programs become bytecode,
which the interpreter runs; hot code becomes machine code through Ixo's own JIT, a
single-pass code generator for x86-64 and ARM64. "Switching to LLVM" means letting LLVM
generate that machine code. The runtime (values, objects, garbage collector, strings,
maps, natives) stays C++ either way; LLVM is itself a C++ library. The options:

- **A. LLVM as the JIT:** replace or extend the current JIT with LLVM's ORC JIT.
- **B. LLVM ahead of time:** `ixo build --native` compiles a whole program to an
  executable, with the runtime linked in.
- **C. LLVM as a top tier:** keep the interpreter and the current JIT, and recompile only
  very hot, type-stable code with LLVM. JavaScriptCore did this in 2014–2016 (FTL), then
  replaced LLVM with its own B3 backend because LLVM compiled too slowly.

## What was measured

`bench/backend/run.py` (Linux, clang/LLVM 21.1, best of 5) times four workloads five
ways:
- **Ixo interpreter** and **Ixo JIT**: today.
- **LLVM, dynamic:** `bench/backend/*_dyn.cpp` compiled by clang -O2. These keep Ixo's
  exact runtime model (`dyn.hpp`): 16-byte tagged values, a type check on every
  operation with an out-of-line fallback, wrapping ints, fields behind a class guard,
  bounds-checked lists, calls through module variables. This is what LLVM produces
  without type specialization, and an optimistic version of it: there are no GC safe
  points, task-switch checks or deoptimization exits, which a real backend needs.
- **g++, dynamic:** the same files by GCC, to see how much depends on the optimizer.
- **LLVM, typed:** the plain C++ versions from `bench/langs`. This is the ceiling: what
  LLVM produces when every type is known.

| workload | Ixo interpreter | Ixo JIT | LLVM, dynamic | g++, dynamic | LLVM, typed |
|----------|----------------:|--------:|--------------:|-------------:|------------:|
| fib      |           0.297 |   0.053 |         0.024 |        0.028 |       0.014 |
| loops    |           0.990 |   0.202 |         0.043 |        0.021 |       0.021 |
| sieve    |           0.598 |   0.147 |         0.088 |        0.077 |       0.016 |
| nbody    |           2.040 |   0.272 |         0.204 |        0.067 |       0.030 |

Seconds, including process start-up (Ixo about 0.01 s). WSL timings vary by 20% or so
between runs; the ratios are stable.

| cost                         | Ixo's JIT                         | LLVM                                    |
|------------------------------|-----------------------------------|-----------------------------------------|
| compiling one hot function   | about 30 µs (300 functions add about 10 ms to a run) | about 65 ms at -O2, 23 ms at -O0 (nbody: 4 functions in 260 / 93 ms) |
| size                         | the whole `ixo` binary: 3.3 MB (Linux), 5.8 MB (Windows, static) | `libLLVM.so`: 138 MB                    |
| targets                      | x86-64, ARM64                     | everything, but not inside the browser playground |

## What the numbers say

1. **The speed comes from types, not from LLVM.**
   - On Ixo's dynamic model, LLVM beats Ixo's JIT by 1.3× (nbody), 1.7× (sieve), 2.2×
     (fib) and 4.6× (loops).
   - It doesn't remove the boxing and type checks; it only sees through them when it can
     prove a type locally. The result depends on luck: GCC compiled the same dynamic
     nbody 3× faster than LLVM did.
   - With types, the same code runs 4–10× faster than Ixo's JIT today. Getting there
     needs type specialization: types from annotations and the checker, or types
     observed at run time, guarded, with a way back to the interpreter when a guard
     fails. That work is needed whatever generates the machine code.
2. **Most programs wouldn't notice.**
   - Strings, maps, JSON, formatting and sorting spend their time in the runtime, which
     is compiled C++ already. That is 7 of the 16 `bench/langs` workloads.
   - A backend changes only the code between those calls.
3. **The costs are large, and they land where Ixo is strong today.**
   - **Start-up and short scripts:** 1000–2000× slower compiles. A program with 300 hot
     functions would spend about 20 s compiling at -O2, unless compilation moves to
     background threads behind a lower tier (option C).
   - **Size and builds:** about 40× the size of Ixo today. LLVM must be built or
     shipped on every platform: a heavy MSYS2 dependency on Windows, longer CI, and an
     API that changes with every LLVM release (every 6 months).
   - **Features Ixo has today, each harder with LLVM:**
     - machine code that stops anywhere and continues in the interpreter at the exact
       instruction (LLVM: stack maps and patch points)
     - a precise, non-moving collector that must find every reference (statepoints, or
       keeping values in memory, which gives up the register gains)
     - tasks that pause inside machine code
     - panics crossing machine-code frames
     - the profiler's address-to-line mapping
     - the debugger
   - **The browser playground** can't run an LLVM JIT: it stays on the interpreter, as
     today.

Julia shows LLVM working well for a dynamic language, but Julia is built around type
inference that compiles whole methods for concrete types. Its users pay a famous
"time to first plot". LuaJIT shows a hand-written trace JIT reaching near-C speed on
numeric loops without LLVM.

## Recommendation

**Don't switch.**
1. **First, type specialization in Ixo's own JIT.** Use types from annotations and the
   checker, plus types observed at run time behind guards that hand over to the
   interpreter, which the JIT already does well. Add a simple register allocator, so
   ints and floats stay in registers in loops. That is where the measured 4–10× is (the
   "numbers in registers" item left from Phase 9). A reasonable target is nbody and loops
   within 2–3× C++, with today's microsecond compiles kept.
2. **Then, if a native build is wanted, consider LLVM ahead of time** (option B) for
   `ixo build --native`:
   - Compile time doesn't matter there.
   - Only the build tool needs LLVM; `ixo` itself keeps its size.
   - The specialized types from step 1 make LLVM's output fast, and the interpreter and
     JIT stay for the REPL, `--watch`, the playground and dynamic code.

To rerun: `python3 bench/backend/run.py --ixo build/linux/ixo` on Linux or macOS with
`clang++`.
