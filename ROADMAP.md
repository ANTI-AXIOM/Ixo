# Roadmap

## Done

### Phase 0: project foundations
- Build scripts: `build.ps1` (Windows) and `build.sh` (Linux, macOS, Git Bash). Both are
  incremental and parallel, and produce a static `ixo` executable.
- A golden-output test suite (`tests/`, `python tests/run.py`) plus tool tests, fuzzers
  and a documentation checker.
- The old tree-walking prototype, its test binaries and logs moved to `archive/`
  (git-ignored). Your original `guidelines.txt` and `Ixo Language Examples.txt` are unchanged.

### Phase 1: a correct core language
- Real scoping and closures. Separate `int` and `float`. Strings, lists, maps, sets, tuples
  and ranges with complete operators.
- `fn foo` without parentheses, implicit returns, default parameters, calling any function
  like a method, and auto-calling zero-argument members.
- Classes: constructor parameters, fields with defaults, `init`, `self`, private methods,
  printing through a `str` method.
- Pattern matching: literals, ranges, alternatives, guards, `as`, types, tuples, lists
  with `..rest`, class destructuring, error kinds.
- Errors as values: `!` handlers, `retry` (with a count and/or a delay), `?` propagation,
  and panics with stack traces.
- Modules: `use`, `in ... use`, `as`, `*` exports, search path (`IXO_PATH`), cycle detection.
- Error messages point at the line and column, show the source line, and suggest names
  ("did you mean ...").

### Phase 2: the new architecture
- Resolver pass: names become slots, upvalues or globals at compile time, and undefined
  names are compile errors.
- Bytecode compiler with exact stack tracking, and a stack VM with inline fast paths.
  About 1.5× faster than CPython 3.14 on `fib(30)` and 1.8× on arithmetic loops.
- Mark-and-sweep garbage collector that only runs at safe points (tested with
  `IXO_GC_STRESS=1`).
- Lightweight tasks (fibers) with a scheduler, flows (channels), `with tasks`, preemption
  in long loops, and deadlock detection.
- Gradual type checker: inference plus annotations; reports only definite errors; annotated
  parameters, returns and fields are also checked at run time.

### Phase 3: usable
- Standard library: math, `vec`/`mat`/`quat` (with 3D transforms), strings, collections,
  `fs`, `os`, `time`, `rng`, `json`, `net` (through `curl`). The collection helpers are
  written in Ixo (`src/prelude.cpp`).
- REPL, `ixo fmt` (comment-preserving and verified to keep the program's tokens identical),
  `ixo test` for `@test` functions, `ixo check`, `ixo disasm`.
- Language server (`ixo lsp`): diagnostics, hover, go to definition, completion, outline,
  formatting.
- VS Code extension (`editors/vscode`): grammar, language configuration, LSP client, and run
  and test commands.
- Docs: language guide, generated stdlib reference, grammar, examples.

### Phase 4: ambitious
- **Parallelism with `par`.** `par f(x)` and `par fn` run calls on other cores. Each worker
  thread has its own VM and heap, so the GC stays single-threaded and there are no data
  races: arguments, closures and results are deep-copied (cycles and shared structure
  kept), a par task sees a snapshot of the module variables it uses, and flows are shared
  between threads. `with tasks`, `await` and deadlock detection work across threads. About
  3.4× faster on 8 logical cores for CPU-bound work (`examples/parallel.ixo`).
- **Memory regions.** `@arena`, `@frame`, `@temp`, `@req`, `@thread`: whatever a block
  allocated and no longer needs is freed when it ends, and escaping values stay alive (so
  regions can't create dangling references). `@cache name` keeps a map across calls,
  `@dual name = x` is a persistent double buffer. `memory()` and `gc()` builtins.
- **Performance.** Inline integer/float fast paths for arithmetic and comparisons, a fused
  `x += k` instruction for locals. About 1.3× faster than CPython 3.14 on `fib(30)` and 2.4×
  on an arithmetic loop (`bench/`).
- **Language features.** Interfaces with required and default methods (nominal with
  `class X is I`, and structural), operator methods (`fn +(o)`, `fn -()`, `fn ==`, `fn <`,
  `fn []`, `fn []=`), enums with data-carrying variants and shared methods (with a
  missing-variant warning for `match`), `defer`, and `x is Class` type tests. Generic
  functions (`fn first(xs: list[T]) -> T`) are checked at each call.
- **Tooling.** Language server: find references, rename (across open files and their
  imports), signature help, and hover/outline/completion for interfaces and enums. A
  sampling profiler (`ixo --profile`, with flame-graph output). A debugger (`ixo dap`):
  breakpoints, step in/over/out, pause, call stack through callbacks, variables with
  expandable values, hover evaluation, stop on panic. The VS Code extension (0.4.0)
  registers the debugger and adds Debug File and Profile File commands.

## Decisions to review

Your spec left these open or was ambiguous. Each is easy to change if you prefer otherwise.

1. **Scope of `=` inside functions.** It creates a local (like Python), unless an
   *enclosing function* has the variable (closures just work, no `nonlocal`). Module
   variables are changed from functions with compound operators (`score += 1`). This
   prevents a function from overwriting a top-level variable by accident.
2. **Placeholder lambdas.** `_` is the single argument. Named placeholders are ordered
   alphabetically (`_a`, `_b`), which makes your `sort(_b < _a)` example sort descending.
   The lambda forms at the nearest *method call* argument (`.map(...)`), so
   `data.map(Point(_.x, 0))` does what you'd expect.
3. **Auto-calling.** Members without parentheses are called when they take no arguments
   (`list.len`, `16.sqrt`, `player.alive`). A bare name on its own line calls a
   zero-argument function (`die`). `x = f` stores the function.
4. **`//` is the n-th root**, following the `SQRT` token in your lexer: `27 // 3 == 3`. There
   is no integer-division operator; `/` always gives a float and `(a / b).floor` gives an
   int. Integer division may be worth an operator of its own.
5. **`-5` is a literal**, so `-5.abs == 5` (as in your primitives test) and `-2 ** 2 == 4`.
6. **Truthiness:** only `nil`, `false` and errors are false. `0`, `""` and `[]` are true.
7. **Return types use `->`**, so a one-line body uses `=`: `fn sq(x) -> int = x * x`.
   Your spec used `->` for both, and the two are ambiguous.
8. **Privacy is per module.** Unmarked functions, classes and methods are usable anywhere in
   their own file, but not from importers. Fields are always public.
9. **`str.join`:** `", ".join(list)` or `list.join(", ")` instead of `str.join(", ", list)`.
10. **`list.take`** removes and returns the *last* item (a stack), like `pop`.
11. **`fork` shares one OS thread; `par` copies.** `fork` tasks are concurrent but not
    parallel. `par` tasks are parallel, but get copies of their inputs and a snapshot of
    module variables, and only share flows. This keeps the language free of data races. The
    cost is copying: a shared-heap design would need a concurrent GC.
12. **Program exit:** the program ends when the main code and every task that can still make
    progress are done. Blocked tasks don't keep it alive; a blocked main is a "deadlock"
    panic.
13. **Interfaces are structural too.** A class that has the methods satisfies the interface
    even without `is`; `is` adds the compile-time check and the default methods.
14. **`x is Class` is a type test.** `is` is identity for everything else.
15. **Regions are safe, not manual.** A region frees only what's unreachable when it ends.
    That gives up "free everything in one step" for the guarantee that no value dangles.

## Phase 5: the flow of writing code

Editor help while typing
- [x] Docstrings of your own functions, classes, interfaces and enums in hover, completion and
      signature help.
- [x] Quick fixes: add the methods an interface requires, add the missing `match` arms for an
      enum, apply "did you mean", add a `use` for an unknown name, export with `*`.
- [x] Inlay hints: inferred types of variables and parameter names at call sites.
- [x] Semantic highlighting: locals, parameters, fields, classes, enum variants and module
      variables in their own colors.
- [x] Snippets and smarter completion: a `match` over an enum fills in every arm; templates
      for `fn`, `class`, `interface`, `enum`, `match`, `@test`.
- [x] Rename and find references across the whole workspace, not only open files.
- [x] Formatter leftovers (`5[0]`, `elif` chains) so format-on-save is safe everywhere.

Running and testing
- [x] Watch mode: `ixo run --watch`, `ixo test --watch`.
- [x] Tests in the editor: a "run" link above each `@test`, VS Code Test Explorer, and a
      readable diff when `assert_eq` fails.
- [x] REPL: line editing, history, tab completion, `:load` and `:reload`.
- [x] Debugger: evaluate any expression, conditional breakpoints, hit counts, logpoints,
      changing a variable's value.

Language friction
- [x] Multi-line blocks (lambdas, `match`, `if`) inside call arguments and brackets.
- [x] Named arguments: `save(path, overwrite: true)`.
- [x] `ixo new <name>`: a project skeleton.

## Phase 6: next additions

Language and types
- [x] Narrowing: after `if x == nil -> return`, `x` is known not to be nil (also `is` tests
      and `match` arms).
- [x] Generic classes (`class Stack[T]`) and interface method signatures checked against the
      classes that implement them.
- [x] Interface extension: `interface Shape3 is Shape`.
- [x] Integer division, slicing with a step, `match` patterns for maps.
- [x] Borrow modes: `p: &T` is a read-only parameter (mutating it is a compile error),
      `&mut T` and `mut` allow changes.

Concurrency and memory
- [x] `select` over several flows, and receive with a timeout.
- [x] Frozen (deeply immutable) values, shared with `par` workers without copying.
- [x] Debugging and profiling `par` workers; a memory profiler (`ixo --memprofile`).

Performance
- [x] Faster core: cached field and method lookups, computed-goto dispatch,
      superinstructions, a compact value representation.
- [x] A JIT for hot code (`$jit` / `$hot`).

Standard library
- [x] The script's own path and directory.
- [x] Regular expressions, date and time parsing/formatting, CSV, subprocesses.
- [x] HTTP client without `curl`, and a small HTTP server.
- [x] A C foreign-function interface.

Ecosystem and project health
- [x] Continuous integration (GitHub Actions, Windows and Linux).
      Also macOS, and the VS Code extension is packaged on every push.
- [x] Package manager: `ixo.toml`, `ixo add <git-url>`, a lock file.
      `ixo init / add / install / update / remove`; git and folder dependencies, installed flat
      in `ixo_modules/` with their own dependencies; exact commits in `ixo.lock`.
- [x] `ixo build`: a program bundled into a single executable.
      A copy of `ixo` with the program's modules and packages appended; they're served from a
      virtual folder, so module lookup works as in the project.
- [x] Releases: prebuilt binaries, installer, VS Code Marketplace packaging.
      A `v*` tag builds Linux, macOS (arm64, x86-64) and Windows binaries and the extension into
      a draft GitHub release; `install.sh` / `install.ps1`; Marketplace and Open VSX publishing
      when their tokens are set as repository secrets.
- [x] Browser playground (Ixo compiled to WebAssembly).
      `playground/`: Emscripten build with threads (so `par` works), editor, examples, input,
      sharing links, formatting, Stop. The test suite passes on WebAssembly (Node.js), and the
      page is checked in headless Chrome, also without COOP/COEP headers (a service worker
      adds them, for GitHub Pages).

## Phase 7: type less, do more

Goals: less to type and read for the same program, one way to write each thing, and good to
excellent speed from plain code without hints. Old spellings keep working in 0.6 with a
warning and a quick fix, `ixo fmt --upgrade` rewrites them, and 0.7 removes them.
(Done in 0.6.0; the guide's "Upgrading from 0.5" section lists every change.)

The three arrows, in one rule: `=` means "is" (`fn f(x) = x * 2`), `:` means "has type"
(`x: int`, `fn f(x): int`), `->` means "then" (`if ok -> go`, `match` arms, `else`, loops).

Calls and functions
- [x] **Parentheses follow the declaration.** A function declared without a parameter list
      (`fn tick`, `fn alive = hp > 0`) is called whenever its name is used: `print(version)`,
      `h.alive`. A function declared with parentheses, even `fn f()`, is only called with
      them; without, it's the function value. `&tick` gives a no-parameter function as a
      value. Built-in methods without arguments (`.len`, `.up`, `.sort`) work like `fn tick`.
      Decided at the declaration, so the compiler emits a direct call: no run-time check.
      Fixes: `print(version)` printing `<fn version>`, `h.hit` running a method by accident,
      and a new required parameter silently turning calls into values. A bare `h.hit` statement
      that does nothing is a warning ("call it: h.hit()"). An interface method and the class
      method implementing it must use the same form.
- [x] **Return types use `:`** like every other annotation: `fn area(w, h): float = w * h`.
      (Changes the original `-> int`.) Function *types* keep their arrow, `fn(int) -> str`,
      which reads as "maps to"; everywhere else `->` means "then".
- [x] **Lambdas use `=`** like functions: `fn(x) = x * 2`. Trailing `-> block` lambdas stay.
- [x] **One export position: right after the name**, for everything: `fn area*(w, h)`,
      `class Rect*`, `enum Color*`, and now **variables and constants**: `limit* = 10`
      (read-only for importers). (Changes the original `fn calc(x)* -> int` position.)
- [x] **`_` lambdas:** the rule stays (the lambda is the argument of the nearest method call;
      plain calls like `Point(_.x, 0)` pass through) and is documented with examples.
      `_a`, `_b` are parameters in alphabetical order.
- [x] **Generic type parameters may have names** when declared: `class Cache[Key, Value]`,
      `fn first[T](xs: list[T]): T`. Undeclared single letters still work in functions.

Operators and literals
- [x] **`//` is integer (floor) division**, as in Python: `7 // 2` is `3`, `-7 // 2` is `-4`.
      Roots become methods: `x.root(3)` (`.sqrt` stays). `~/` goes.
- [x] **`-2 ** 2` is `-4`**: unary minus binds looser than `**` and `.`, as in maths and
      Python. `(-5).abs` for the method on a negative number; `-5.abs` gets a warning.
- [x] **Ranges: `..` (excludes the end) and `..=` (includes it)**; `..<` goes.
- [x] **A step is written `by`**: `0..=100 by 5`, `xs[.. by -1]`, `xs[0..10 by 2]`. The
      comma is then free for `m[i, j]` on matrices. (Changes the original `0..=100, 5`.)
- [x] **`else` needs no arrow**: `size = if x > 3 -> "big" else "small"`. Continuation
      lines (`elif`, `else`) may be indented anywhere deeper than the statement; no alignment.
- [x] **One brace escape in f-strings: `{{` and `}}`**; `\{` goes.

One spelling for each thing
- [x] `nil` (not `null`), `and` `or` `not` (not `&&` `||` `!x`), `is` / `is not` (not
      `===` `!==`). `!` then means only errors (`x ! e -> ...`, `int ! Error`) and `!=`.
- [x] No `var`: `x: int = 3` declares a typed variable.
- [x] `T ! E` for results, and `T!` as short for `T ! Error`; `Result[T, E]` goes.
- [x] `500ms.wait` (not `sleep(500)`); `$jit` for functions and loops (`$hot` goes),
      `$nojit` to keep one in the interpreter.
- [x] Regions: `@arena` for a scoped region, plus `@cache` and `@dual`, which really behave
      differently. `@temp`, `@frame`, `@req` and `@thread` behaved exactly like `@arena`, so
      they go; a comment says what a block is for. (Changes the original list.)
- [x] `!` on a method name means "the in-place twin of a method that returns a copy"
      (`sort` / `sort!`, `rev` / `rev!`, `uniq` / `uniq!`, `mix` / `mix!`). Methods that only
      change the value (`add`, `drop`, `take`, `wipe`) keep their plain names. Discarding the
      result of a copying method is a warning: "`l.sort` makes a sorted copy; `l.sort!` sorts
      in place".

Classes
- [x] **Fields without `self.`**: inside a method, a field's name means the field
      (`fn len = (x ** 2 + y ** 2).sqrt`, `hp -= damage`). Parameters and locals shadow
      fields (`self.x` still works); a field that hides a module variable gets a warning.
      Methods work by their bare names too, except where a built-in has the name (`json`,
      `print`): there `self.json` is the method.
- [x] **Fields come from the constructor parameters, the class body and `init`**, and nowhere
      else. A class's layout is then fixed when it's compiled, so every field access is a
      direct slot (no lookup, no cache) and a typo in any method is a compile error.

Concurrency
- [x] Calls to `par fn` / `fork fn` functions return tasks. The checker reports a task used
      as a value ("add .await"), and editors show a `task` hint at those calls.
- [x] `fn +=(o)` (and the other compound operators) on classes update in place; without it,
      `a += b` keeps meaning `a = a + b`.
- [x] The profilers report how much `par` copied into tasks, pointing at `freeze`.

Errors
- [x] `Error.Kind` stays free-form (no declaration needed), but matching a kind that nothing
      in the program or the standard library creates is a warning with "did you mean".

Loops
- [x] `for i, x in list` gives the index and the item (as maps give key and value), with no
      tuple made per iteration; `for (a, b) in pairs` unpacks, and `for i, (a, b) in pairs`
      does both. `.indexed` stays for other uses.

Performance: fast without hints
- [x] Lambdas passed to the built-in list methods (`map`, `keep`, `each`, `fold`, `sum`,
      `any`, `all`, `count`, `find_by`, `sort_by`, `min_by`, `max_by`) are compiled into the
      loop itself: no closure and no call per element. (Your original "`_` is zero-allocation"
      goal.)
- [x] Calls to functions the compiler can see (module functions, methods of a known class,
      `x.f(a)` written for `f(x, a)`) are bound at compile time.
- [x] The JIT compiles list indexing, field reads and writes (fixed slots), method calls on
      known classes, and float arithmetic without boxing, instead of handing those to the
      interpreter. Then ordinary code with lists and objects gets machine-code speed.
      Also: calls between compiled functions (and `x.f(a)` calls), loops over lists, and code
      after an instruction it doesn't compile (loops anywhere in a script). A call that stops
      deep inside machine code continues in the interpreter exactly where it was, and long
      loops still let other tasks run. Sorts with `_a < _b`-style comparators compare
      directly.
- [x] An ARM64 JIT backend (Apple Silicon, ARM Linux). The same code generator over an
      AArch64 assembler; tested under qemu on Linux (the test suite and every JIT check agree
      with the interpreter). The Apple silicon path (MAP_JIT pages) is built and run by CI
      only.

Migration
- [x] 0.6: old spellings give a warning with a quick fix; `ixo fmt --upgrade` rewrites files;
      examples, tests and docs move to the new forms. 0.7: old spellings are errors.

## Phase 8: performance

Goal: plain code, written without hints, runs faster than Python and close to C++ for
numbers. (Done; the version stays 0.6.0, since nothing in the language changed. Measured
on the same machine with `python bench/langs/run.py --runs 5 --no-jit`.)

### Results

| workload  |   C++ | Ixo before | Ixo now | now, no JIT | Python | Ixo vs Python now |
|-----------|------:|-----------:|--------:|------------:|-------:|-------------------|
| loops     | 0.020 |      0.093 |   0.078 |       0.572 |  1.124 | 14.5× faster      |
| fib       | 0.007 |      0.024 |   0.026 |       0.154 |  0.143 | 5.5× faster       |
| nbody     | 0.023 |      1.503 |   0.254 |       1.410 |  1.160 | 4.6× faster       |
| closures  | 0.031 |      0.234 |   0.100 |       0.259 |  0.311 | 3.1× faster       |
| objects   | 0.108 |      0.771 |   0.264 |       0.546 |  0.749 | 2.8× faster       |
| sort      | 0.087 |      0.273 |   0.244 |       0.274 |  0.567 | 2.3× faster       |
| sieve     | 0.014 |      0.107 |   0.116 |       0.372 |  0.255 | 2.2× faster       |
| wordcount | 0.074 |      0.474 |   0.196 |       0.261 |  0.429 | 2.2× faster       |
| jsonio    |   n/a |      1.129 |   0.297 |       0.301 |  0.403 | 1.4× faster       |
| format    | 0.233 |      0.830 |   0.290 |       0.330 |  0.305 | 1.1× faster       |
| strings   | 0.055 |      1.313 |   0.127 |       0.156 |  0.133 | 1.1× faster       |
| **total** | 0.651 |      6.752 |   1.990 |       4.635 |  5.581 | 2.8× faster       |

Targets:
- [x] Faster than Python on every workload. format and strings only just: their time is
      mostly the runtime library, about even with CPython's C code.
- [ ] Numeric workloads within 10× C++. Met by loops, fib, sieve and objects; nbody is at
      11×. Its loop now runs entirely as machine code, but every float lives in a 16-byte
      boxed slot. Keeping floats in registers across instructions would close the gap
      (a register allocator: a later phase).
- [x] strings, format and jsonio within 1.5× of Python (all three are now faster).
- [x] No workload slower with the JIT than without (jsonio is even: 0.297 vs 0.301).

### Starting point (0.6.0, before this phase)



Measured with `python bench/langs/run.py --runs 5 --no-jit`. It runs 11 workloads, each
written idiomatically in Ixo, C++ (g++ 15.2 -O2) and Python 3.14, and checks that all three
print the same result. Windows 11, i3-1215U, Ixo 0.6.0. Times are the best of 5 runs in
seconds and include process start-up (Ixo 0.011 s, Python 0.025 s).

| workload  | what it does                             |   C++ |   Ixo | Ixo, no JIT | Python | Ixo vs Python |
|-----------|------------------------------------------|------:|------:|------------:|-------:|---------------|
| loops     | integer arithmetic, 10M iterations       | 0.017 | 0.093 |       0.512 |  1.114 | 12× faster    |
| fib       | fib(30), recursive calls                 | 0.007 | 0.024 |       0.106 |  0.138 | 5.7× faster   |
| sieve     | primes to 2M, list reads and writes      | 0.015 | 0.107 |       0.323 |  0.234 | 2.2× faster   |
| sort      | 500k ints, 125k objects by key, comparator | 0.082 | 0.273 |     0.266 |  0.532 | 2.0× faster   |
| closures  | map/keep/fold/count, calling a closure   | 0.024 | 0.234 |       0.203 |  0.302 | 1.3× faster   |
| objects   | 1M small objects, methods                | 0.114 | 0.771 |       0.754 |  0.694 | 1.1× slower   |
| wordcount | map with 1M string-key updates           | 0.070 | 0.474 |       0.460 |  0.420 | 1.1× slower   |
| nbody     | 5-body simulation, 200k steps, floats    | 0.022 | 1.503 |       1.527 |  1.200 | 1.3× slower   |
| format    | 300k f-strings with `.2f`, parse back    | 0.228 | 0.830 |       0.814 |  0.308 | 2.7× slower   |
| jsonio    | encode and parse 100k records            |   n/a | 1.129 |       1.159 |  0.383 | 2.9× slower   |
| strings   | join, upper, split, search, `s +=`       | 0.049 | 1.313 |       1.326 |  0.127 | 10× slower    |
| **total** |                                          | 0.629 | 6.752 |       7.451 |  5.452 | 1.2× slower   |

Pure computation is fast: 3–7× C++ and up to 12× faster than Python. Anything that
allocates, calls a built-in or handles text is slower than Python. The JIT gives almost
nothing on 7 of the 11 workloads. WSL (Linux) shows the same pattern.

### Bottlenecks found (largest first)

1. **The JIT exits at every built-in call.** Native functions and built-in methods (`sqrt`,
   `.add`, `.len`, `.abs`), class constructors, f-strings (`FORMAT`, `CONCAT`), inline
   `map`/`keep` appends (`LIST_APPEND`) and map indexing all exit to the interpreter. The
   interpreter runs until the next loop header, then re-enters. After 100 such exits on a
   loop, on-stack replacement gives up and the loop stays interpreted.
   - nbody: JIT on 1.50 s vs off 1.53 s. Removing only the `d2.sqrt` call drops the time to
     0.52 s, so 2M `sqrt` calls cost about 1 s, roughly 500 ns each.
   - objects (building the list) and closures are slower with the JIT than without (build
     step: 0.317 s vs 0.229 s).
2. **`for i in a..b` allocates under the JIT.** Each loop start makes an `ObjRange` and an
   `ObjIterator`. nbody's inner `for j in i + 1..count` allocates 2.6M objects (230 MB) and
   causes 7 collections. Rewriting those loops as `while` loops halves the JIT time (0.52 s →
   0.25 s with `sqrt` removed). Fixing points 1 and 2 together puts nbody near 0.25 s: 5×
   faster than Python, about 11× C++.
3. **Every object is its own `malloc`, and most kinds take several.** An `ObjString` is about
   72 bytes plus the allocator's header. An `ObjInstance` keeps its fields in a separate
   `std::vector` (2 allocations). An `ObjMap` uses a `std::unordered_map` (one node per
   key) next to an entries vector. Every object also has a vtable and is freed with a
   virtual `delete`. Creating a string costs about 200 ns on Windows (`"k" + v`: 236 ns vs
   Python's 39 ns). A constructor call `Point(x, y)` costs about 320 ns (Python: about 165).
   This shows up in objects, wordcount, format, jsonio and strings.
4. **`s += x` copies the whole string each time.** A loop of 30k appends that builds a 60 KB
   string takes 1.14 s (Python: 29 ms, because CPython resizes in place). That is 87%
   of the strings workload. Also, `f"a{i}b"` makes one string per part and then a joined
   one: 3 allocations instead of 1.
5. **The garbage collector re-marks the whole live heap on every collection.** In strings,
   600k live strings are marked again 15 times: 740 ms of collection in a 1.3 s run. Large
   programs that keep data alive pay this cost over and over.
6. **Text and number conversion is slow.**
   - `float(s)`: 545 ns vs Python's 140 ns. It copies the string 3 times, then calls
     `strtoll` and then `strtod`.
   - `f"{x:.2f}"`: 580 ns vs 330 ns (printf-based).
   - `s.cut("|")`: 570 ns vs 94 ns (one list plus one new string per piece).
7. **JSON goes through an intermediate C++ tree in both directions.** Parsing builds a
   `Json` tree and then converts it to Ixo values: 0.52 s vs Python's 0.115 s. Encoding
   converts values to a tree and then writes text: 0.26 s vs 0.115 s.
8. **Closures that capture variables can't be compiled.** In closures, 44% of the time is
   spent in the `compose` lambda, which runs in the interpreter ("uses variables from an
   enclosing function").
9. **The measuring tools mislead.** `--profile` charges all machine-code time to loop
   headers: nbody shows 96% on `for` lines, while the cost is `sqrt`. `--memprofile` records
   a string before its characters are counted: the append loop above reports 2.7 MB
   allocated when the real figure is about 900 MB. `IXO_JIT_TRACE` stops after 50 lines and
   shows no totals.

### Work done

- [x] **Measure correctly first**:
  - `IXO_JIT_STATS=1` lists where machine code hands over to the interpreter (function,
    line, instruction) and which functions or loops the JIT gave up on.
  - `--profile` now runs with the JIT on. The interpreter is sampled before any
    instruction, not only at loops and calls. Machine code is sampled by its address, read
    by the timer thread (Windows) or a signal handler (Linux, macOS), and mapped back to
    the line. Time inside helpers goes to the line that called them, found through the
    return address on the stack.
  - `--memprofile` counts string characters and list growth.
  - `IXO_GC_STATS=1` prints each collection's mark and sweep times.
  - `tests/tools/profile_check.py` (in CI) checks the profiler, memprofile and JIT stats.
- [x] **Call built-ins from machine code**:
  - Natives and built-in methods are called directly (`jitNative`).
  - Constructors allocate the instance and run their compiled `init`.
  - Other instructions that don't jump are run for the machine code by the interpreter,
    one at a time, on the same fiber (`jitStep`). The common ones skip the dispatch loop
    (`jitFastOp`): indexing, operators on strings and objects, f-strings, literals,
    appends. If such an instruction would need a call to an Ixo function, it gives up
    before changing anything, and the machine code stops there instead, so the fiber's
    stack never moves under it.
  - A task that waits inside one of these pauses cleanly and resumes after the
    instruction.
  - A panic inside a helper is raised again once the interpreter's frames are rebuilt,
    so traces stay exact.
  - `and`/`or`, truthiness of any value, default arguments, error checks and
    `JUMP_IF_NOT_SEQ` (inlined `map`/`keep`...) are compiled too.
  - `x.sqrt` and int `x.abs` are inline machine code.
  - Not done: inline `min`/`max`/`floor`/`.len`. They are direct native calls now, and
    profiles showed no need.
- [x] **Counted loops without objects**: `for i in a..b [by s]` keeps start, end and step
  in hidden slots (`RANGE_INIT`, `FOR_RANGE`). It checks its bounds exactly as ranges do,
  in the interpreter and the JIT.
- [x] **A faster object heap**:
  - Objects up to 512 bytes come from per-VM free lists in 16-byte size classes. Frees of
    strings, instances and lists avoid the virtual call.
  - Instance fields live in the same allocation as the instance.
  - Maps and sets with up to 8 keys have no hash index (a scan is faster). Bigger ones use
    an open-addressing index of (hash, position) slots over their entries, in place of
    `std::unordered_map`.
  - Not done: string characters in the same allocation (short strings already need no
    second allocation, and `->s` is used in 139 places), and dropping the vtable.
- [x] **Cheap string building**:
  - `s += x` (and `s = s + x`) grows the string in place in two cases. For a local that a
    loop only grows and never reads, the loop's entry seals the string (`STR_APPEND`,
    `STR_SEAL`). For a module variable, it grows until something reads the variable;
    every read seals it (`STR_APPEND_G`).
  - Strings remember which of the two may grow them, so a string can't be grown through
    an alias.
  - Numbers in the same position (checker-typed int or float locals) keep the plain
    `+=`.
  - An f-string is built in one buffer, its parts evaluated in order (`FSTR_*`).
- [x] **Fast number text**:
  - `int()`/`float()`/`.num` parse with `from_chars` when the text is plain.
  - f-string `f`/`e`/`g` specs format with `to_chars`; JSON numbers use `to_chars`.
  - `cut` sizes its result once.
- [x] **Direct JSON**:
  - `json.parse` builds Ixo values while reading; map keys repeated across records share
    one string. `json.str` writes straight from values.
  - Side effects: integers up to 2^63 are exact both ways (they used to go through
    doubles), floats print in the shortest form that reads back (0.1, not
    0.10000000000000001), and big objects no longer have quadratic key handling.
- [x] **Captured variables in the JIT**: functions that use variables of an enclosing
  function compile (methods excepted). The machine code takes the running closure from its
  frame, so every closure of the function sees its own variables.
- [ ] **Generational collection**: evaluated and not done. `IXO_GC_STATS` shows why it isn't
  worth it yet:
  - Most collection time was re-marking data that programs were still building up. An
    adaptive heap target removes that: when a collection frees less than a quarter of the
    heap, the next one waits for 4× the live size instead of 2×. jsonio and strings now
    collect once; objects collects twice. The heap can be up to 4× the live data (capped
    at 1 GB of headroom).
  - What remains is mostly sweeping dead young objects. A non-moving young generation
    doesn't make that cheaper; only a copying one would.
  - A write barrier is needed at every store into an existing object: interpreter, JIT
    and well over 100 sites in the natives. Big growing lists also need card marking.
    That is a large and risky change. Collection is now 4–14% of these workloads' time,
    except objects (about 22%: a million long-lived instances are marked twice).

## Phase 9: memory and time

Goal: use no more memory than Python, close to C++ where data dominates, without losing
speed. (Done, apart from what is noted below; the version stays 0.6.0.)

### Results

Best of 5 runs on Windows with `python bench/langs/run.py --runs 5 --no-jit`. The machine
was quieter than for the "starting point" table further down, so compare times through
the Python column (Python's own times moved too: 10.9 s then, 7.6 s now).

| workload  | MB before | MB now | MB Python | MB C++ | RAM now vs Python | time now | time Python |
|-----------|----------:|-------:|----------:|-------:|------------------:|---------:|------------:|
| hello     |       6.2 |    6.3 |       9.7 |    3.3 |       0.6×        |    0.015 |       0.087 |
| loops     |       7.1 |    6.5 |       9.8 |    3.3 |       0.7×        |    0.077 |       1.115 |
| fib       |       7.1 |    6.4 |       9.8 |    3.3 |       0.7×        |    0.027 |       0.151 |
| nbody     |      26.0 |    6.7 |      10.2 |    3.8 |       0.7×        |    0.233 |       1.193 |
| closures  |      45.6 |   45.1 |      71.8 |   23.4 |       0.6×        |    0.103 |       0.304 |
| floats    |      38.7 |   38.5 |      98.3 |   20.1 |       0.4×        |    0.157 |       0.345 |
| intlist   |     134.6 |  134.5 |     235.5 |   68.1 |       0.6×        |    0.123 |       0.797 |
| sort      |      48.2 |   43.0 |      56.4 |   14.5 |       0.8×        |    0.225 |       0.612 |
| strings   |      74.0 |   53.2 |      57.0 |   31.9 |       0.9×        |    0.105 |       0.158 |
| records   |     104.2 |   82.0 |      95.1 |   33.4 |       0.9×        |    0.136 |       0.316 |
| jsonio    |     161.3 |  104.5 |     110.4 |    n/a |       0.9×        |    0.255 |       0.383 |
| wordcount |      32.9 |    9.8 |      10.2 |    4.0 |       1.0×        |    0.189 |       0.459 |
| strmap    |      91.7 |   70.6 |      70.2 |   39.7 |       1.0×        |    0.245 |       0.356 |
| format    |      37.9 |   10.4 |       9.8 |    3.8 |       1.1×        |    0.261 |       0.302 |
| objects   |     201.1 |  127.8 |     110.6 |   26.6 |       1.2×        |    0.234 |       0.763 |
| sieve     |      38.3 |   37.0 |      25.0 |    3.8 |       1.5×        |    0.102 |       0.256 |
| **total** |    1054.8 |  782.5 |     989.8 |  283.3 |       0.8×        |    2.486 |       7.596 |

Ixo now uses 26% less memory than before and 21% less than Python in total. It runs 3.1×
faster than Python in total (2.8× before). Linux (WSL) agrees: 798.8 MB against Python's
929.0. 1000 tasks that wait while running machine code went from 51.4 MB and 0.99 s to
15.8 MB and 0.13 s (11.5 MB with the JIT off).

Targets:
- [ ] Peak memory at most Python's on every workload. Met on 12 of 16. strmap is even
  (70.6 / 70.2) and format is 0.6 MB over, which is the runtime's own start-up footprint.
  Two remain above:
  - **objects** (1.2×): a million live instances are 64 B each, and the discarded copies
    wait for the next collection (Python frees them at once).
  - **sieve** (1.5×): a list item takes 16 B, Python's 8.
  - Both need 8-byte values (below).
- [x] Within 2× C++ where data dominates: intlist 2.0×, floats 1.9×, strmap 1.8×. sieve
  is the exception: C++'s `vector<bool>` stores bits.
- [~] 1000 waiting tasks use about as much memory with the JIT as without: 15.8 MB
  against 11.5 MB (it was 51.4 MB).
- [x] No workload slower: every one is the same or faster relative to Python.

### Starting point (before this phase)

`bench/langs/run.py` now reports the **peak memory** of every run as well as its time.

That is the most RAM the process used at once: the peak working set on Windows, the maximum
resident set on Linux and macOS. Five workloads were added:
- **hello:** start-up and base footprint.
- **intlist:** 5M ints in a list.
- **floats:** 2M floats.
- **records:** 300k objects, each holding a string and a two-item list.
- **strmap:** a map with 500k string keys.

These are best-of-5 results on the same Windows machine. This session's timings were about
30% slower than Phase 8's for all three languages (C++ included: the machine was busier),
so compare the ratios, not the seconds.

| workload  | time C++ | time Ixo | time Python | Ixo/C++ | MB C++ | MB Ixo | MB Python | Ixo/Python RAM |
|-----------|---------:|---------:|------------:|--------:|-------:|-------:|----------:|---------------:|
| hello     |    0.007 |    0.017 |       0.032 |    2.3× |    3.3 |    6.2 |       9.7 |       0.6×     |
| loops     |    0.021 |    0.096 |       1.585 |    4.6× |    3.3 |    7.1 |       9.7 |       0.7×     |
| fib       |    0.008 |    0.036 |       0.184 |    4.4× |    3.3 |    7.1 |       9.7 |       0.7×     |
| closures  |    0.030 |    0.126 |       0.399 |    4.2× |   23.4 |   45.6 |      69.5 |       0.7×     |
| floats    |    0.033 |    0.247 |       0.474 |    7.6× |   20.1 |   38.7 |      98.9 |       0.4×     |
| intlist   |    0.069 |    0.231 |       1.094 |    3.4× |   68.1 |  134.6 |     235.5 |       0.6×     |
| sort      |    0.146 |    0.304 |       0.844 |    2.1× |   14.5 |   48.2 |      56.4 |       0.9×     |
| records   |    0.090 |    0.195 |       0.438 |    2.2× |   33.5 |  104.2 |      95.1 |       1.1×     |
| strings   |    0.073 |    0.164 |       0.166 |    2.2× |   31.8 |   74.0 |      56.9 |       1.3×     |
| strmap    |    0.432 |    0.242 |       0.715 |    0.6× |   39.6 |   91.7 |      70.4 |       1.3×     |
| sieve     |    0.017 |    0.157 |       0.315 |    9.3× |    3.8 |   38.3 |      25.0 |       1.5×     |
| jsonio    |      n/a |    0.529 |       0.533 |       - |    n/a |  161.3 |     110.0 |       1.5×     |
| objects   |    0.149 |    0.541 |       1.126 |    3.6× |   26.5 |  201.1 |     110.6 |       1.8×     |
| nbody     |    0.025 |    0.350 |       1.805 |   14.1× |    3.8 |   26.0 |      10.2 |       2.6×     |
| wordcount |    0.118 |    0.248 |       0.719 |    2.1× |    4.0 |   32.9 |      10.2 |       3.2×     |
| format    |    0.326 |    0.419 |       0.497 |    1.3× |    3.8 |   37.9 |       9.8 |       3.9×     |
| **total** |    1.545 |    3.904 |      10.926 |         |  282.8 | 1054.8 |     987.4 |                |

Timings this noisy vary by 20–40% between passes: jsonio took 0.529 s here, and 0.375 s
(Python 0.554) in a 7-run recheck. Memory is stable to within 1%.

Time is in good shape: Ixo is faster than Python everywhere and 2–5× C++ on most
workloads. Memory is not:
- Ixo uses more RAM than Python on 9 of the 16 workloads, and up to 10× C++.
- The runtime itself is small: 6.2 MB for `hello`, against 9.7 MB for Python.
- The extra comes from the size of values and objects, and from when garbage is freed.

Object sizes measured with `sizeof`, against CPython 3.14 (`sys.getsizeof`):

| thing                    | Ixo                          | CPython                     | C++ |
|--------------------------|------------------------------|-----------------------------|-----|
| a value in a list or field | 16 B (tag + payload)       | 8 B pointer (+ the object)  | 8 B |
| instance with 2 fields   | 56 + 2×16 = 88 B (96 B class) | 48 B                       | 16 B |
| string `"name12345"`     | 80 B                         | 50 B                        | 32 B |
| map with 5 keys          | 96 B + 8×32 B entries = 352 B | 184 B                      | |
| object header            | 32 B (vtable, flags, size, list link) | 16 B (refcount, type) | 0 |

### Memory bottlenecks (M1–M7, largest effect first)

1. **The collector waits for 32 MB before its first collection.** Programs with little live
   data still grow to about 32 MB of garbage: format 37.9 MB (Python 9.8), wordcount 32.9
   (10.2), nbody 26.0 (10.2).
   - Measured with an experiment build starting at 4 MB: format 10.6 MB, wordcount
     10.6 MB, nbody 11.6 MB.
   - Time didn't change measurably, because collecting a small live set is cheap.
2. **Values take 16 bytes:** a tag byte padded to 8, then the payload. Every list item,
   field, map entry and stack slot is twice a pointer.
   - intlist: 134.6 MB, against 68.1 for C++.
   - sieve: 38.3 MB, against Python's 25.0.
   - Packing values into 8 bytes (NaN-boxing: doubles as they are, everything else in
     the spare NaN bits) would halve all of them. Every `Value` access and the JIT's
     slot layout change.
3. **Object headers are 32 bytes, and objects carry redundant parts:**
   - A vtable pointer: freeing already switches on the type.
   - A link in the list of all objects: the pools could be swept page by page instead.
   - A 32-byte `std::string` inside every string, plus a separate buffer above 15
     characters.
   - A data pointer next to an instance's inline fields.
   - With a 16-byte header, inline characters and no field pointer: a 2-field instance
     would take 56 B (48 B with 8-byte values), and `"name12345"` 48 B.
4. **Containers keep growth slack.**
   - Map entries and list items grow by doubling and never shrink.
   - A 5-key map literal or JSON object has room for 8 entries: 256 B of entries for
     160 B of data.
   - jsonio: 161.3 MB against Python's 110.0. Its 100k records hold 5-entry maps, 3-item
     lists and many identical one-character strings (Python shares those).
5. **Garbage that follows a big live set waits for the heap to double or quadruple.**
   - objects peaks at 201 MB with 115 MB live: the 1M discarded `moved` points pile up.
   - Phase 8's adaptive headroom (4× after a collection that freed little) made it
     slightly worse. Capping it didn't help here, because the whole run allocates
     213 MB.
   - Only freeing short-lived objects early does: a young generation, or reference
     counting.
6. **`for x in list` allocates an iterator object each time the loop starts.** nbody makes
   200k of them (19.8 MB of garbage, plus the allocation time). A list loop can keep the
   list and a position in hidden slots, as counted range loops now do.
7. **Tasks that run machine code grow their stack to fit 2000 nested calls.**
   - 1000 tasks running a compiled function use 51.4 MB, against 11.5 MB with the JIT
     off (Python's asyncio: 25.0 MB).
   - The stack could instead grow when machine code actually reaches its end: it would
     leave to the interpreter, which grows the stack, then re-enter.

### Time bottlenecks (T1–T6)

1. **A task that waits loses its machine code.**
   - `10ms.wait` (any pause) inside a compiled function makes the machine code leave
     cleanly. That exit counts as a failure, though.
   - After 50 such exits the JIT gives up on the function and its loops.
   - 1000 tasks each running a 20k-iteration loop and then a wait: 0.986 s with the JIT,
     0.983 s without.
   - A pause should count like the turn given to other tasks, which isn't a failure.
2. **Floats live in 16-byte slots in memory.** Every float operation loads, checks, computes
   and stores through the frame.
   - nbody: 14.1× C++. floats: 7.6× C++.
   - Keeping a loop's numbers in CPU registers (type-specialized code with register
     allocation) is the next step for numeric speed.
3. **Hot built-in methods go through the generic native call.**
   - `xs.add(v)` costs about 40 ns. It is 87% of intlist's time, 3.4× C++.
   - Inline machine code for `list.add`, `len`, `map.has`/`get` and `str.len` would
     help: append in place when there is room, else the helper.
4. **`[x] * n` and `s * n` build their result without reserving room.** sieve spends 38% of
   its time in `[true] * (n + 1)`. A one-line fix (reserve the size, fill).
5. **Sweeping costs time per dead object** (about 20 ns each).
   - format runs 6 collections of about 8 ms: about 12% of its time.
   - Sweeping pool pages with mark bitmaps (no linked list) would make it cheaper.
   - So would a young generation, where dead objects cost nothing, but moving objects
     isn't possible today: machine code and natives hold raw pointers.
6. **Small container buffers come from malloc:** list and tuple items, map entries, strings
   over 15 characters. records spends 87% of its time on the line that builds each record
   (an object, an f-string and a two-item list).
   - Pooling small buffers like objects would help.
   - So would keeping lists of up to 4 items inline in the object.

### Work done

- [x] **Quick wins:**
  - The collector runs first at 4 MB (format 37.9 → 10.4 MB, wordcount 32.9 → 9.8 MB).
    Above 32 MB of live data it runs when the heap has grown by half, not doubled
    (objects 150 → 128 MB, at no measured time cost).
  - A task that pauses inside machine code no longer counts against the JIT.
  - `[x] * n` reserves its size (`s * n` already did).
- [x] **List loops without iterator objects**: `SEQ_INIT` and `FOR_SEQ` keep the list and a
  position in hidden slots, for `for x in xs` and the inlined `map`/`keep`/`fold`/...
  - Other sequences still get their iterator, through the same code (`VM::iterStep`).
  - Keyed loops (`for i, x in`) and unpacking loops keep `ITER_INIT`.
  - nbody: 26.0 → 6.7 MB.
- [x] **Stack room on demand**: machine code starts with room for 16 nested compiled calls
  per fiber. A fiber that gets deeper exits to the interpreter (which grows the stack),
  and next time gets 4× the room, up to 2000.
- [x] **Exact-size containers**:
  - Map and set literals reserve their size.
  - The JSON parser builds every array and object in a shared scratch buffer, then
    allocates it exactly. It also shares one string for repeated keys and short values.
  - Lists give memory back when they fall below a quarter of their room. `wipe` frees
    it all.
  - A string grown in place gives back its spare room when sealed.
  - jsonio: 161 → 105 MB.
- [x] **Inline hot built-ins in the JIT**: `xs.add(v)` appends in machine code when the list
  has room, with the same accounting as the helper. `.len` of lists and maps is inline.
  `map.has`/`get` stay direct native calls: they hash anyway. String `.len` counts
  characters, so it stays a native call too.
- [x] **Smaller objects**:
  - The header is 12 bytes: no vtable (a type switch runs destructors) and no
    all-objects list. The collector walks the pool chunks slot by slot; free slots are
    marked `Free`. Objects over 512 B live in a separate list.
  - `@arena` regions log what they allocate instead of relying on list order.
  - A string's hash is 32 bits, in the header's spare bytes.
  - Instance fields sit at a fixed offset after the instance (no pointer; the JIT
    computes the address).
  - Sizes: string 80 → 48 B (CPython: about 50), instance 56 → 32 B + 16 per field,
    list 56 → 40 B, map 96 → 80 B, closure 64 → 48 B. records 104 → 82 MB, strings
    74 → 53 MB, strmap 92 → 71 MB.
- [ ] **Small buffers from pools**: not done. The workloads it was for (records, jsonio) now
  use less memory than Python. Their remaining allocation time is a few percent, which
  doesn't justify replacing the global allocator.
- [ ] **8-byte values (NaN-boxing)**: not done, deliberately.
  - Ixo's ints are full 64-bit. With 8-byte values, either ints beyond about 2^50 or
    floats would have to live on the heap. Workloads with large intermediate ints
    (wordcount, sort, any hashing) or floats (nbody) would allocate on every operation
    and get slower, against this phase's "no workload slower".
  - The change also touches about 430 places that read values and the JIT's slot layout
    on both CPUs.
  - It is the one change left that would bring objects and sieve under Python's memory.
    A narrower alternative is lists that store unboxed ints, floats or bools when all
    their items are of one kind.
- [ ] **Numbers in registers**: not done. Type-specialized machine code with register
  allocation is a compiler project of its own (a phase). nbody stays at 11× C++, floats
  at 5.9×.
- [~] **Free short-lived objects early**: the pacing change above handles the measured case
  (objects 201 → 128 MB). A young generation or reference counting remains as discussed
  in Phase 8.

## Phase 10: typed containers (done)

Goal: lists and instance fields that hold only numbers or bools take 1–8 bytes per item
instead of 16, without changing what any program means. This is the step that takes
memory to Python's or below on every workload: sieve is at 1.5× (16-byte list items) and
objects at 1.2× (16-byte fields). It also brings data-heavy work close to C++.

Why containers and not variables: a function has a handful of locals, and machine code
finds each at `slot × 16`. Narrowing locals saves almost nothing and adds a lookup to
every access. Mixed widths inside one list would break `xs[i]` (item i at
`start + i × size`). So the width is chosen per container, from what it holds, as V8
("elements kinds") and PyPy ("list strategies") do.

### Design

**Kinds.** A list's kind is stored in the object header's spare byte, so the list costs
nothing extra:

| kind   | holds                     | per item | when another value arrives |
|--------|---------------------------|---------:|----------------------------|
| empty  | nothing yet (`[]`)        |        - | becomes the new value's kind |
| bools  | only `true`/`false`       |   1 byte | values                     |
| ints   | only ints (full 64-bit)   |  8 bytes | values                     |
| floats | only floats               |  8 bytes | values                     |
| values | anything (today's layout) | 16 bytes | -                          |

- An int list doesn't become a float list when a float arrives: `1` and `1.0` are
  different values (`x is int`), so any mix becomes `values`.
- Widening converts the items once, in O(n). A list never narrows back: no flip-flopping.
- `nil` in a list (common as a placeholder) makes it `values`. A nullable-int kind was
  considered and left out: not worth a sixth kind until measurements say so.

**Storage.** The list keeps one union: either today's `std::vector<Value>` or a raw buffer
(data, size, capacity) for the typed kinds. Both are 24 bytes, so `ObjList` stays 40 bytes.

**Access, in two levels, so the change can land safely in steps:**
- **Typed fast paths:** `len`, `get(i)`, `set(i, v)`, `push(v)`, iteration, slices.
  They never widen except to store a value that doesn't fit, and they hand out values,
  never references.
- **`view()`:** read-only access to any kind without widening (size, `[]` by value,
  range-for). The read-only natives move to it: printing, `join`, `has`, `sum`,
  `max`/`min`, comparisons, `json.str`, `par` copies, `freeze`. Printing a big int
  list must not quietly turn it back into 16-byte values.
- **`items()`:** today's `std::vector<Value>&`. Asking for it widens the list to
  `values`, permanently, so every reference it gives out stays valid. Every one of the
  about 140 places that use lists today keeps working through `items()` from day one,
  and they move to the faster levels one by one.

**Where kinds are chosen:**
- **Literals** (`[1, 2, 3]`), `[x] * n`, `(a..b).list`, `map`/`keep` results,
  `json.parse` arrays and `str.cut` results get the kind of their contents.
- **`[]`** starts `empty`, and its first `add` decides.
- **Typed fast paths:** `sum` of an int list is a tight loop over raw ints; `sort` of an
  int or float list compares raw numbers.

**Machine code:**
- Indexing, `for x in xs`, `xs.add(v)` and `.len` check the kind byte and take a raw
  path per kind. A typed read gives an int or float directly, with no tag check.
- The current inline paths become the `values` case.

**Collector:** typed lists hold no references, so marking skips their items entirely. A
list of 5M ints is no longer scanned at every collection.

**Unchanged:** tuples (small and fixed; see Phase 11 for their layout), sets, maps.

### Typed instance fields (second part)

Classes learn each field's representation, as V8 does for object fields:
- A field that has only ever held ints (or floats, or bools) is stored raw in 8 bytes
  (bools: 1). Annotations (`x: int`) fix the representation from the start.
- When a field receives another type, the class switches that field to a 16-byte value
  for all its instances. They are converted when next touched or collected, guarded by
  a layout version in the class.
- Instances get a size per class layout: a `Point(x, y)` of ints goes from 64 to 48 bytes
  (Python: 48 plus its int objects). objects would drop from 128 MB to about 110 MB.
- This is harder than lists: layout changes affect existing instances, inline caches and
  the JIT's fixed field offsets. Do it only if objects still matters after typed lists.

### Steps

Each step ships with all tests, the JIT check and GC stress passing.

1. [x] `ObjList` gets the kind byte, the union storage and the three levels (`get`/`set`/
   `push`/`len`, `SeqView` for read-only access to lists and tuples, `items()` widening).
2. [x] Kinds on creation (`newList` picks the narrowest kind, `[]` starts `empty`,
   `[x] * n` copies packed storage directly) and in the interpreter (`LIST_APPEND`,
   `FOR_SEQ`, iterators, indexing). `tests/collections/typed_lists.ixo` covers the
   widening rules, `1` vs `1.0`, `nil`, `-0.0`, NaN and hot loops.
3. [x] Read-only natives on `SeqView`; `add`, `len`, `get`, `set`, `take`, `has`, `find`,
   `slice`, `copy`, `rev!` on typed paths; `sort` of an int list (or a float list without
   NaN) sorts raw numbers. Editing natives (`insert`, `drop`, `remove`, `sort!`) work on
   values and store the result back at its narrowest kind.
4. [x] JIT paths per kind (indexing, `.len`, inline `add`, `for x in xs`); the collector
   skips typed lists; `par`, `freeze`, JSON, CSV, the debugger and printing read through
   `SeqView`, so none of them widen a list.
5. [x] Evaluated 4-byte ints and bit-packed bools. Not done: intlist is already at C++'s
   memory (70.4 MB against 68.1) and sieve's bools take 2 MB of its 8.4 (bits would save
   1.75 MB), while both would add a kind and a slower element access to every JIT path.
6. [x] Objects: instead of typed fields, two layout changes that help every object. The
   pools use 8-byte size classes (were 16), and instances and tuples keep their item
   count in the 4 header bytes alignment used to waste: a 2-field instance takes 56
   bytes (was 64), a pair 48 (was 64). Above 64 MB live, the collector runs when the
   heap has grown by a third (was a half). Typed instance fields move to Phase 12, where
   type profiles and guards make a field's representation a natural thing to specialize.

Results (Windows, best of 3–5):

| workload | before: s / MB | after: s / MB | Python: s / MB | C++: s / MB |
|----------|---------------:|--------------:|---------------:|------------:|
| sieve    | 0.157 / 38.3   | 0.057 / 8.4   | 0.328 / 25.0   | 0.018 / 3.8 |
| intlist  | 0.231 / 134.6  | 0.110 / 70.4  | 1.068 / 234.1  | 0.064 / 68.1 |
| floats   | 0.247 / 38.7   | 0.200 / 22.5  | 0.478 / 97.4   | 0.036 / 20.1 |
| sort     | -     / 34.4   | 0.189 / 34.4  | 0.781 / 56.4   | 0.105 / 14.4 |
| objects  | 0.311 / 127.8  | 0.315 / 105.2 | 1.003 / 110.5  | 0.140 / 26.6 |
| tuples probe | 0.232 / 89.0 | 0.296 / 74.5 | 0.330 / 141.5 | - |

- Peak memory is now at most Python's on every workload except format (10.3 MB against
  9.8) and strmap (70.6 against 70.2), which are level.
- sieve, intlist and floats are within 1.12× C++'s memory. sieve and intlist are within
  3.2× and 1.7× its time.

Targets:
- Peak memory at most Python's on every workload.
- sieve, intlist and floats within 1.5× C++.
- No workload slower; big scans and collections faster.

## Phase 11: runtime efficiency (done)

A search for more problems like Phase 10's: costs that come from how data is represented
and handled rather than from the code a program runs. Each probe below runs in Ixo and
Python (Windows, best of 3, seconds and peak MB). The probes are in `bench/probes`; run
them with `python bench/probes/run.py [names]` (the two string probes take about a
minute each until item 1 is done).

| probe                                        | Ixo s  | Ixo MB | Python s | Python MB |
|----------------------------------------------|-------:|-------:|---------:|----------:|
| `s[i]` over a 200k-char string               | 67.812 |    9.5 |    0.036 |      10.0 |
| 500k `text.slice(k, k + 12)` of an 88 KB string | 61.292 |  45.6 |    0.119 |      45.5 |
| 10k tasks that wait 50 ms                    |  0.193 |   56.5 |    0.367 |      36.1 |
| 100k tasks that wait 50 ms                   |  6.430 |  501.1 |    1.759 |     175.8 |
| 8 `par` tasks on 4 workers (JIT on / off)    | 0.074 / 0.033 | 10.7 | 0.438 (processes) | 19.5 |
| 1M tuples `(i, 2i)` in a list                |  0.290 |  121.0 |    0.251 |     141.6 |
| 300 small hot functions: memory, JIT on / off | -      | 10.2 / 8.5 | -     | -         |
| 500k closures, each with its own variable    |  0.188 |   78.8 |    0.640 |     169.1 |
| 1M-entry int map and set                     |  0.334 |   85.0 |    0.300 |     178.5 |
| 50k lines × 2 regex operations               |  0.159 |   23.2 |    0.148 |      16.3 |

The last three are fine: closures and int maps already use half Python's memory, and
regular expressions are on par. The rest, worst first:

1. [x] **Strings know they are ASCII.**
   - Today `.len`, `s[i]` and `s.slice(a, b)` scan the whole string, to count UTF-8
     characters and check for non-ASCII bytes, on every call. Loops over a string by
     position are therefore quadratic: 1900× slower than Python for `s[i]`, 500× for
     `slice`.
   - Fix: a flag in the string's header (its spare byte: unknown / ASCII / not ASCII),
     computed once. An ASCII string (most strings) then has O(1) `.len`, indexing and
     slicing.
   - Non-ASCII strings count characters once. If indexing them still shows up, they can
     get a small index of positions every 64 characters (Python uses fixed-width
     storage instead).
   - Indexing or iterating a string also creates a new one-character string each time.
     A table of the 128 ASCII one-character strings, made once, removes those
     allocations (Python does the same).
   - Target: both probes on par with Python.
2. [x] **The scheduler scales.**
   - Sleeping tasks are kept in a vector, scanned in full at every turn to find the next
     wake-up time and the tasks due, and each woken task is removed by another linear
     search. The fiber list and run queue are searched the same way when a task ends.
     So 10× more tasks takes 29× longer.
   - Fix: a min-heap by wake-up time, and constant-time removal (each fiber knows its
     position; finished fibers are skipped, not searched for).
   - Each task also starts with a 4 KB stack (256 slots) plus about 600 bytes of frame
     storage: about 5 KB per task against Python's 1.8 KB. Start stacks at 32 slots
     (512 bytes) and grow on demand, as they already can.
   - Target: 100k tasks at or below Python in time and memory.
3. [x] **`par` workers stop counting toward compilation.**
   - Workers never compile, but their loops still increment the hotness counters of the
     functions they share with the main program. Four threads writing one cache line
     cancel the parallel speedup (0.074 s with the JIT on, 0.033 s with it off). It's
     also a data race.
   - Fix: worker VMs skip all JIT bookkeeping.
4. [x] **Tuples store their items inline.**
   - A tuple keeps its items in a separate `std::vector`: a second allocation and about
     40 extra bytes per tuple.
   - Tuples never change size, so the items can follow the object, as instance fields
     do since Phase 9.
   - Expected: 1M pairs from 121 MB to about 80 MB, one allocation instead of two, faster
     than Python. Pairs are everywhere: `zip`, `indexed`, map `pairs`, multiple return
     values.
5. [x] **Machine code shares pages.**
   - Each compiled function gets its own executable allocation: at least one 4 KB page,
     and a 64 KB address reservation on Windows. That's about 5.7 KB per function
     (300 functions: 1.7 MB).
   - Fix: a code arena that packs functions into shared pages, for about 0.5–1 KB per
     function. Large programs with thousands of hot functions save megabytes.

Results after the five fixes (same machines and runs as the table above):

| probe        | before: s / MB  | after: s / MB | Python: s / MB |
|--------------|----------------:|--------------:|---------------:|
| strindex     | 67.812 / 9.5    | 0.033 / 6.7   | 0.045 / 9.9    |
| slices       | 61.292 / 45.6   | 0.090 / 39.0  | 0.156 / 45.4   |
| tasks (10k)  | 0.193 / 56.5    | 0.109 / 22.8  | 0.585 / 35.5   |
| tasks100k    | 6.430 / 501.1   | 0.307 / 165.0 | 2.397 / 175.7  |
| partasks     | 0.074 / 10.7    | 0.049 / 10.5  | 0.687 / 19.1   |
| tuples       | 0.290 / 121.0   | 0.296 / 74.5  | 0.330 / 141.5  |
| manyfns (MB) | 10.2            | 9.4           | -              |

- Small task stacks keep two frames' worth of slots for machine code (`Fiber::jitDepth`),
  so 100k tasks use 165 MB, not 510.
- `IXO_JIT_LOG` prints each compile's time and why a function stayed interpreted;
  `IXO_JIT_STATS` adds the total compile count and time (Phase 15's first step).

Not problems:
- Measured: regex (compiled patterns are cached), closures and int-keyed maps (half
  Python's memory), small maps (no hash index since Phase 8).
- Estimated, not measured: the bytecode's line table (4 bytes per bytecode byte, a few
  hundred KB even for large programs).

Suggested order: 1, 2 and 3 first (each is small, and 1 and 2 fix quadratic behavior),
then 4 and 5. They are independent of Phase 10 and can go before or after it.

## Phase 12: typed native code (mostly done: see "What was built")

Goal: code whose types are known runs as plain machine code, with no tags, no type checks
and numbers in registers: parity with C++ on scalar code. Today nbody is 11× C++ and
loops 4.4×; `bench/backend` measured 4–10× from types alone. This phase also completes
Phase 9's deferred "numbers in registers". It needs Phase 10's typed lists, and no LLVM:
it extends Ixo's own JIT.

### Where types come from

1. **Annotations and the checker.** `fn step(b: Body, dt: float)`, `xs: list[float]`.
   The checker already infers types from literals, annotations and the standard library;
   its results are recorded on each function for the compiler.
2. **Observed types.** The interpreter and the baseline JIT record, at each operation,
   the types they have seen (a few bits per site: int, float, bool, str, list kind,
   instance of class C). After enough runs, a function is compiled again, assuming those
   types.
3. **Guards.**
   - An assumption the checker can't prove is checked once, at function or loop entry.
     If it fails, the code returns to the interpreter at that exact instruction, as the
     JIT already does today, and the function's profile is updated.
   - A function whose guards keep failing goes back to the baseline JIT. Results never
     change, only speed.

### The compiler: a second JIT tier

- **Tiers:**
  - **Tier 0:** the interpreter.
  - **Tier 1:** today's JIT. A single pass, microsecond compiles, every value in its
    16-byte slot.
  - **Tier 2 (new):** for functions and loops that stay hot with stable types.
- **Typed IR in SSA form.** Each value has one definition and a machine type (i64, f64,
  bool, pointer to an object of class C, typed-list pointer), built from the bytecode
  and the type information above.
- **Optimizations, in the order they pay:**
  1. Unboxing: ints and floats live in registers, not in 16-byte slots. A value goes
     back into a slot only where the interpreter could see it (exits, calls into it).
  2. Inlining of small functions and methods, including calls through module variables
     and inline caches that have always seen one target.
  3. Removing redundant checks: one type guard per value, not per use; one class check
     per object per loop.
  4. Bounds-check elimination: `for i in 0..xs.len` proves `xs[i]` is in range.
  5. Escape analysis: an object that never leaves a function (a temporary `Point`, a
     tuple returned and unpacked at once) lives in registers and is never allocated.
  6. Loop-invariant code motion: field and length loads hoisted out of loops when
     nothing in the loop can change them.
- **Register allocation:** linear scan, simple and fast to compile, on x86-64 and ARM64
  (the backends share the IR).
- **Exits back to the interpreter:** every guard knows how to rebuild the interpreter's
  frame from registers: which register or constant holds each slot. This is the
  "deoptimization metadata" that today's JIT doesn't need, because its slots are always
  in memory.
- **Compile time:** still the reason not to use LLVM. Target: under 1 ms for a typical
  function, compiled on a background thread while tier 1 keeps running.

### What doesn't change

Untyped and polymorphic code keeps running in tiers 0–1, exactly as now. Panics, traces,
the debugger and the profiler keep exact lines: the IR keeps each operation's bytecode
offset.

### Steps

1. [x] Type profiles, in the interpreter only (tier 1 doesn't count anything). Each
   instruction whose result type the code doesn't fix (fields, items, calls, module and
   captured variables, loop items) records the kinds it produced, in a byte at the offset
   of the instruction after it; parameters get one byte each. Indexing also records "the
   container wasn't a list". `IXO_JIT_STATS` counts typed compiles and demotions.
2. [x] Types per slot and instruction, computed over the bytecode (a dataflow pass,
   `inferTypes`), rather than a separate SSA IR: ints, floats, bools, nil, "some value"
   and "maybe unset". Loops compiled before the interpreter ever ran them have no
   profile: the first time machine code reaches one, it hands it to the interpreter
   once, which learns its types, and the function is compiled again. No IR printer:
   `IXO_JIT_DUMP=dir` writes the machine code instead (`objdump -D -b binary`).
3. [x] Unboxing, guards and exits. Typed values live in registers between
   instructions; memory is brought up to date where control flow joins, before
   anything the interpreter might see and on every exit (each exit writes back exactly
   the registers that differ from memory at that point). A guess that turns out wrong
   hands over to the interpreter and is counted; after 64 (and 1 in 16 calls) the
   function is compiled again without guesses. `IXO_JIT_DEOPT_STRESS=1` makes every
   guess fail and runs the whole suite that way, on x86-64 and ARM64.
4. [x] Register allocation, not linear scan: a per-instruction allocator over 8 general
   and 12 float registers (least recently used goes first), copies of variables that
   share their register until the variable changes, and "homes": each innermost loop
   keeps its busiest typed variables (counters, sums, bounds) in fixed registers across
   iterations. Every way into one of the loop's join points brings them there; code
   arriving with everything in memory enters through a small loading stub. A dry first
   compile finds which callee-saved registers are used, so only those are saved.
5. [ ] Of the optimizations:
   - [x] Check elimination for objects: a variable once checked to be an instance of
     class C keeps its pointer in a register, unchecked, until it changes. Frozen is
     checked once per variable too.
   - [x] Division and modulo by a constant: a multiplication and shifts (verified on
     76 million cases against floor semantics).
   - [x] Typed lists in loops: `for x in xs`, `xs[i]`, `xs[i] = v`, `xs.add(v)` read and
     write the packed storage directly; `s += x` on numbers adds in registers.
   - [x] Direct calls: machine code calls a closure's machine code without a helper;
     captured variables are read in machine code.
   - [ ] Inlining, bounds-check elimination, escape analysis, loop-invariant code
     motion: not done. fib and callbacks are what they would help most.
6. [ ] Background compilation for tier 2: see Phase 15. Tier-2 compiles take 0.1–1 ms
   (two passes); not yet needed.

### What was built: results

`IXO_JIT_TIER2=0` turns the typed tier off. Times on the same machine, best of 3 or 5,
including start-up (Ixo's is about 0.01 s); the machine was noisy, so ±15%:

| workload | tier 1 | tier 2 | C++ | Python | tier 2 / C++ |
|----------|-------:|-------:|----:|-------:|-------------:|
| loops    | 0.074  | 0.029  | 0.020 | 1.24 | 1.4× |
| nbody    | 0.227  | 0.056–0.064 | 0.030 | 1.23 | 2.1× |
| floats   | 0.150  | 0.049–0.065 | 0.034 | 0.44 | 1.4–1.9× |
| sieve    | 0.042  | 0.026–0.033 | 0.022 | 0.28 | 1.2–1.5× |
| intlist  | 0.082  | 0.066–0.076 | 0.051 | 0.98 | 1.3–1.5× |
| fib      | 0.026  | 0.023–0.026 | 0.011 | 0.16 | 2.4× |
| callbacks | 0.166 | 0.087–0.097 | 0.023 | 0.97 | 3.8× |

- Every test passes in every mode (typed, tier 1 only, every guess failing, GC stress)
  on Windows x86-64, Linux x86-64 and ARM64 (qemu); the JIT check agrees on every program.
- Building it found an old ARM64 bug: tier 1's floor modulo tested the flags of an `eor`,
  which sets none on AArch64 (now fixed and tested).

Targets:
- nbody, loops and fib within 1.5× C++, sieve and floats within 2× (with typed lists).
- No program slower than tier 1.
- Tier-2 compiles under 1 ms each.

## Phase 13: beyond C++ (in progress: steps 2, 3 and 6 done in part)

Goal: be faster than typical C++ where Ixo's rules give the compiler facts C++ can't
promise, as Fortran (no overlapping arrays), ISPC (vectorized by default) and Julia
(specialized at run time) do. Builds on Phases 10 and 12.

### 1. No aliasing by construction

- C++ must assume that two pointers might point into the same memory unless the
  programmer promises otherwise (`__restrict`). That blocks reordering and vectorizing.
- Ixo has no pointers, and several facts can be proven:
  - a list or object made in the function, not stored anywhere yet, is unique;
  - frozen values never change;
  - two different typed lists never share storage (a typed list's buffer belongs to
    that list alone).
- Where the compiler can't prove two lists differ, it checks once before the loop (one
  pointer comparison) and keeps two versions of the loop.
- Language support:
  - `freeze` for large read-only data (already there);
  - a checker-proven "pure" mark on functions with no side effects (inferred, not
    written by the programmer);
  - optionally, value structs that are copied, never shared: a later language
    decision, kept out of this phase unless measurements call for it.

### 2. Vectorization by default

- Loops and the inlined `map`/`keep`/`fold`/`sum` over typed lists of ints, floats or
  bools compile to SIMD: 2–8 numbers per instruction (SSE2/AVX2/AVX-512 on x86-64,
  NEON/SVE on ARM64).
- Reductions (sum, min, max, count) use several partial results. A float sum then adds
  in a different order, which can change the last bits of the result. So it applies to
  floats only where the program allows it, with an explicit `xs.sum_fast` or a module
  setting; ints are always exact.
- Conditionals inside loops become masked operations; `vec` and `mat` math uses SIMD
  registers.

### 3. The real CPU

- The JIT checks the CPU's features once at start-up (CPUID on x86-64, the system's
  feature flags on ARM64) and uses the widest SIMD available.
- A shipped C++ binary usually targets a baseline CPU unless built with `-march=native`.
  This is an advantage that only a JIT has.
- `ixo build` executables keep it, since they JIT on the machine they run on.

### 4. Specialization at run time

- Values that stay the same while code runs (a function passed as a callback, a
  configuration constant, a format string, a regex, the shape of JSON records) are
  folded into the machine code behind a guard.
- `xs.sort_by(_.age)` inlines the key; a `compose`d function inlines both parts.
- C++ needs templates written for this, or profile-guided builds.

### 5. Parallel by default

- `par` exists. In this phase, a `map`/`each`/`fold` of a pure function over a large
  typed list splits across cores by itself (above a size threshold, measured).
- A `par for` loop form for explicit cases.
- Results are identical to the single-threaded order. Reductions combine in a fixed
  order, so ints and exact operations match bit for bit; floats follow the same
  `sum_fast` rule as above.

### Steps

1. [ ] Alias facts: not done. The typed tier reads packed list storage directly, but
   doesn't yet reorder or vectorize loops, which is what alias facts would allow.
2. [x] (in part) SIMD for whole-list operations on packed lists (`src/simd.cpp`): `sum` of
   ints, `min`, `max`, `has`, `find`, and `sum_fast` for floats (several partial sums;
   `sum` keeps the program's order, so its result is exactly a loop's). AVX2 or SSE4.2 on
   x86-64, NEON on ARM64, plain loops elsewhere; `IXO_SIMD=none|sse4.2` forces narrower
   versions, and the suite passes with each. SIMD code generated by the JIT for loops:
   not done.
3. [x] CPU features are detected once (`os.cpu` shows `{arch, simd, cores}`); the list
   operations pick their version from it. The JIT doesn't use AVX yet.
4. [ ] (in part) Specialization: `sort_by(_.field)` reads the field without calling the
   lambda (the compiler marks such lambdas), and int keys sort without boxing; comparators
   like `sort(_a.age < _b.age)` already worked that way. Guarded inlining of callbacks:
   not done (it needs Phase 12's inlining).
5. [ ] Purity and automatic `par`: not done. `par` workers run in the interpreter (each
   has its own heap; sharing the main heap's machine code would race on its counters),
   so splitting a JIT-compiled `map` across workers would be slower, not faster, until
   workers can run machine code of their own.
6. [x] A benchmark set against idiomatic C++, in `bench/langs`: `callbacks`
   (`std::function`), `sortby` (`std::stable_sort` with a lambda), `hashmap`
   (`std::unordered_map`), `reduce` (`<numeric>`/`<algorithm>`).

Building the benchmark set found that maps with int keys were slow: their index mixed
keys with a multiplication, so even consecutive keys landed in random slots and every
lookup missed the cache (112–297 ns in plain C++). The index now works like Python's
dict: an int is its own hash and probing mixes in the high bits, so patterned keys
still spread (`tests/collections/map_keys.ixo`): 5–27 ns.

| workload  | before | now   | C++   | Python | now / C++ |
|-----------|-------:|------:|------:|-------:|----------:|
| reduce    | 3.71   | 0.446 | 0.841 | 5.22   | 0.5× (faster) |
| strmap    | 0.243  | 0.286 | 0.426 | 0.39   | 0.7× (faster) |
| sortby    | 0.218  | 0.104–0.113 | 0.118 | 0.23 | 0.9–1.0× |
| hashmap   | 0.289  | 0.157 | 0.062 | 0.49   | 2.5× |
| format    | 0.328  | 0.306 | 0.243 | 0.41   | 1.3× |
| callbacks | 0.166  | 0.087 | 0.023 | 0.97   | 3.8× |

C++ here is built with `-O2` for the baseline CPU, as a shipped binary usually is: that is
why `reduce` loses (no AVX2), and the point of steps 2–3.

Targets:
- Faster than idiomatic C++ on the new benchmark set and on today's `strmap`/`format`.
- Within 1.2× of hand-tuned C++ on loops, sieve and floats.
- Never different results.

## Phase 14: GPU compute (in progress: steps 1–4 and 7 done)

Goal: write GPU code in Ixo itself: kernels in a restricted Ixo, data in typed device
buffers, launched like tasks. Data-parallel work (arrays, images, physics, simulation,
ML-style math) then runs 10–100× faster than on the CPU, in any language.

### The language

```ixo
gpu fn saxpy(a: float, x: buffer[float], y: buffer[float])
    i = gpu.index                      # this thread's position
    if i < y.len -> y[i] = a * x[i] + y[i]

x = gpu.buffer(xs)                     # a typed float list, uploaded as is
y = gpu.buffer(ys)
saxpy(2.0, x, y).await                 # one thread per item; a task like `par`
result = y.list                        # back to the CPU
```

(Proposed syntax, to be settled with the language guide.)

- **`gpu fn`:** a kernel. The checker enforces the GPU subset:
  - Allowed: ints, floats, bools, `vec`/`mat`/`quat`, structs of those (classes whose
    fields are all such types), `buffer[T]`, fixed-size local arrays, loops,
    conditionals, math, calls to other `gpu fn`.
  - Not allowed: strings, maps, lists, closures, tasks, panics with messages,
    recursion. Each is a compile error with a clear message ("a gpu fn can't use a
    str").
- **`gpu.index`** (1D) and `gpu.index2`/`gpu.index3`; work-group size and shared memory
  (`gpu.shared[float](256)`) and barriers (`gpu.sync`) for advanced kernels.
- **`gpu.buffer(list)`, `gpu.buffer[float](n)`:** device memory. Uploads and downloads are
  explicit (`.list`, `.upload(xs)`) or happen when a buffer is read on the CPU. Buffers
  are freed by the collector, or at once with `.free`.
- **Launches return tasks:** `.await`, `with tasks`, mixing GPU and CPU work and flows
  work as they do for `par`. Several launches queue in order on the device.
- **Pointers:** typed buffers and indices are the safe default; that's how GPU code is
  written anyway. An `unsafe gpu` block can hand out raw device addresses and do
  pointer arithmetic, for custom layouts and interop with C/CUDA libraries through
  `ffi`. It is marked in the source and reported by `ixo check`, like Rust's `unsafe`.
- **Indexing outside a buffer:**
  - With checking on (the default): the kernel records the first bad index and the
    launch's task fails with a normal panic message.
  - `$nocheck` kernels skip the check for full speed.

### The compiler

- `gpu fn` bodies go through Phase 12's typed IR (types are all known by the subset's
  rules), then to a GPU backend. All are written in-house, no LLVM:
  - **SPIR-V (Vulkan compute):** a binary IR with a public specification. It runs on
    NVIDIA, AMD and Intel GPUs on Windows and Linux, and on macOS through MoltenVK. The
    first and main target.
  - **WGSL (WebGPU):** text, the simplest to generate, and the only one that also runs
    in the browser playground.
  - **PTX (CUDA):** text, for NVIDIA-only features and peak speed. Later, optional.
- Kernels are compiled when first launched and cached by their argument types.

### The runtime

- Device discovery and choice (`gpu.devices`, `gpu.use(n)`); a CPU fallback that runs
  kernels on all cores when there is no GPU, so programs run anywhere, slower.
- Buffers with host/device copies, mapped memory where the platform shares it (Apple
  silicon and integrated GPUs: no copies).
- Command queues, fences and a completion thread that wakes waiting tasks.
- Profiling: `--profile` shows kernel time and transfer time.

### Dependencies

- The Vulkan loader (system-provided with GPU drivers on Windows and Linux; MoltenVK on
  macOS), loaded at run time. `ixo` still runs without it: `gpu.devices` is empty and
  the CPU fallback is used.
- WebGPU in the browser comes with the browser.
- Nothing new at build time.

### Steps

1. [x] The `gpu fn` subset (`src/gpu.cpp`): checked when the kernel is compiled, with an
   error that says what isn't allowed (`tests/compile_errors/gpu_subset.ixo`). Parameters
   are typed `int`, `float`, `bool`, `buffer[int]`, `buffer[float]`; on the device, ints and
   floats are 32-bit (as in WGSL), and `//` and `%` floor as everywhere in Ixo. The kernel
   becomes a small typed tree that every backend takes. `gpu` is a word only before `fn`
   (it stays usable as a name).
2. [x] Buffers (`gpu.floats`, `gpu.ints`, `gpu.buffer`; `.len`, `.list`, `.upload`, `[i]`,
   `.free`) live in memory the device maps, so the CPU reads and writes them in place. A
   call launches one thread per item of the first buffer (`gpu.launch(n, f, ...)` for an
   explicit count) and gives a finished task: launches run in order. Indexing outside a
   buffer stops the program with the index and the buffer's name. The CPU fallback runs the
   kernel as register code on all cores, with the same results (`IXO_GPU=cpu`).
3. [x] SPIR-V generation (`src/spirv.cpp`, written from the specification: no SDK) and a
   Vulkan runtime (`src/vk.cpp`: the loader opened at run time, the few structures compute
   needs declared from the specification). Tested on an Intel UHD (Windows) and Mesa's
   lavapipe (Linux). Kernels: saxpy, Mandelbrot, matrix multiply, ints and floored division,
   loops with break/continue, math (`tests/gpu/kernels.ixo`, run on the GPU and on the CPU).
4. [x] (in part) WGSL generation (`gpu.wgsl(kernel)`), checked in a real WebGPU:
   `tests/tools/wgsl_check.mjs` runs five kernels in headless Chrome and compares with
   Ixo's own results. Running kernels from the browser playground: not done (the
   playground's interpreter would need to wait for WebGPU, which is asynchronous).
5. [ ] Shared memory, barriers, 2D/3D launches; `vec`/`mat` in kernels: not done. (Launches
   past 65535 work groups already use a second dimension internally.)
6. [ ] `unsafe gpu`, `ffi` interop and PTX: not done.
7. [x] `bench/gpu`: Mandelbrot, saxpy and a matrix multiply, on the GPU, on the CPU fallback,
   as plain Ixo and as C++ (`-O2`, one core). On an Intel UHD Graphics (integrated):

| workload | GPU | CPU fallback (8 threads) | plain Ixo (JIT) | C++ |
|----------|----:|-------------------------:|----------------:|----:|
| Mandelbrot 1024×768, 256 iterations | 0.004–0.007 s | 0.79 | 0.40 | 0.17 |
| saxpy, 16M floats | 0.014 | 0.12 | 0.15 | 0.013–0.016 |
| matrix multiply 512×512 | 0.010 | 1.69 | 4.9 | 0.061 |

- The GPU is 25–40× faster than one C++ core on Mandelbrot and 6× on the matrix multiply;
  saxpy is bound by memory, which an integrated GPU shares with the CPU.
- The CPU fallback interprets register code. It is about 8× faster than the first version,
  which walked the tree, but still slower than plain Ixo for Mandelbrot: compiling kernels
  to machine code for the CPU (with Phase 12's code generator) is the next step there.
- Results: the CPU fallback matches C++'s 32-bit floats exactly. GPUs round some float
  operations differently (fused multiply-adds), so a few pixels of a Mandelbrot edge differ,
  as they do between any two devices; ints are exact everywhere.

Targets:
- Kernels within 1.5× of hand-written CUDA/GLSL equivalents.
- Every GPU program gives the same results on the CPU fallback (exact for ints; floats
  within the GPU's documented precision).
- No cost for programs that don't use the GPU.

Dependencies between phases: 10 (typed lists) comes before 12. Phase 13 needs 12; Phase 14
needs 12's typed IR, and uses 10's typed lists for zero-copy buffers. Phase 11 is
independent and can go at any time. Phase 15 can start before 12 (warm start and adaptive
thresholds work with today's JIT); its background compilation and tier policy pay off
with 12's second tier.

## Phase 15: automatic compilation (in progress: steps 1–3 done, 4–5 wait for Phase 12)

Goal: hot code gets compiled at the right time, at the right tier, without pausing the
program and without the programmer ever writing a hint. Work done in an earlier run is
not lost. This is the compiler's counterpart of the garbage collector: it watches the
running program and decides by itself.

### Today

The JIT is already automatic:
- Every function and loop has a hotness counter. A function is compiled after about
  200 calls, a loop after about 2000 iterations (entering the running loop:
  on-stack replacement).
- Functions called from machine code are compiled at once.
- Code that keeps handing back to the interpreter is given up on (pauses and turns for
  other tasks don't count).
- `$jit` (compile now) and `$nojit` (never) are optional overrides; `IXO_JIT=0` turns it
  off.

What is missing:
- Compiling happens on the program's own thread.
- There is only one tier.
- Every run starts cold.
- The thresholds are fixed numbers.

### 1. Background compilation

- **A compiler thread per VM** (the main VM's only; `par` workers don't compile). A
  function or loop that crosses its threshold is queued. The program keeps running in
  the interpreter or the current tier, and the new code is installed when ready: at the
  function's next call, or the loop's next turn.
- **Safety:**
  - The compiler thread reads only what doesn't change once compiled (bytecode,
    constants, line tables), plus a snapshot of the type profile and inline caches,
    taken when the request is queued.
  - The functions and constants being compiled are pinned (kept as roots) until done,
    so the collector never frees them underneath.
  - Installing is one pointer store, done by the VM thread.
- **Policy:**
  - Today's tier compiles in microseconds and may stay on the VM thread, if
    measurements show the queue isn't worth it.
  - Phase 12's tier 2 always compiles in the background.
  - Stale requests are dropped: functions no longer hot, or already compiled.
- **The browser playground** has no JIT, so nothing changes there.

### 2. Tiers promote and demote by themselves

- **Promotion:** tier-1 code that stays hot with stable types is queued for tier 2:
  roughly 10k calls or 100k loop turns, tuned by measurement.
- **Demotion:** tier-2 code whose guards keep failing goes back to tier 1. It is
  profiled again and may be recompiled later with the wider types it saw, at most a
  few times per function (as loop entries are capped at 8 recompiles today).
- **Discarding:** code that is no longer used is freed. This needs Phase 11's code arena,
  which can free space, unlike today's one-allocation-per-function memory.

### 3. Warm start: remembering hot code between runs

- **Saved at exit, into a profile, for each function that got hot.** A function is
  identified by module path, name and a hash of its bytecode. The profile records:
  - how hot it got and which of its loops were hot;
  - the types seen at each operation;
  - the tier reached.
- **Where:** a cache folder (`.ixo-cache/` next to `ixo.toml` in a project, else the
  user's cache directory), capped in size, oldest entries first out.
- **At start-up:** functions found in the profile are compiled ahead, in the background:
  tier 1 at once, tier 2 with the recorded types behind the usual guards. A changed
  function has a different hash and is simply ignored, so stale profiles never change
  results.
- **`ixo build`:** executables embed the profile of a training run
  (`ixo build --train args...`), so a shipped program starts warm.
- **Not cached:** machine code itself. Compiling from a profile takes microseconds per
  function, and stored code would need relocation and validation. Reconsider only if
  measurements show start-up compile time matters.
- **Control:** `IXO_JIT_CACHE=0` (or a setting in `ixo.toml`) turns it off. `ixo cache
  clean` empties it.

### 4. Adaptive thresholds

- **By size:** small functions are cheap to compile and are compiled sooner; large ones
  wait longer.
- **By load:** while the compiler thread is idle, thresholds drop; under load, they rise.
- **By what the loop will do:** a loop whose trip count is known at entry (`for i in 0..n`
  with a large `n`, `for x in xs` over a long list) is compiled when it starts, rather
  than after 2000 turns.
- **Decay:** counters decay over time, so code that was busy only during start-up isn't
  compiled long after it stopped mattering.
- **Tuning:** all of it is set by measurement over `bench/langs`, `bench/probes` and the
  test suite: time to result, not just steady-state speed.

### 5. Visibility

- `IXO_JIT_LOG`: each compile with its tier, reason (hot, loop entry, warm start,
  promotion, demotion), time spent and thread.
- `IXO_JIT_STATS`: time spent compiling, per tier; queue lengths; warm-start hits and
  misses.
- `--profile`: compile time shown apart from run time.

With warm start and adaptive thresholds, `$jit` is rarely useful. It stays as an override,
and the guide says when it still helps (a one-off measurement with no profile yet).

### Steps

1. [x] Visibility first: `IXO_JIT_LOG` prints each compile's time and reason (called
   often, a hot loop, `$jit`, called from machine code); `IXO_JIT_STATS` adds the number
   of compiles and their total time.
2. [x] Adaptive thresholds, measured and not adopted. A sweep of call/loop thresholds
   (50/500, 100/1000, 200/500, 200/2000, 500/2000, 1000/5000) over `bench/langs`,
   `bench/probes/manyfns` and the test suite found every setting within noise (bench
   2.32–2.39 s in total). Compiling is cheap and both thresholds are low, so the
   interpreted calls before compiling cost almost nothing. The thresholds stay fixed
   (`kJitCalls` = 200, `kJitTurns` = 2000, in `vm.hpp`), and so does decay, which had no
   measured case to fix.
   - What did matter was compile cost itself. Installing each function flipped page
     protection twice, which cost more than compiling a small function. The code arena
     is now mapped twice (a writable view and an executable view of the same memory:
     `CreateFileMapping` on Windows, `memfd` on Linux, with the old path as fallback).
     300 small functions compile in 6.2 ms instead of 16.7, and manyfns runs in 0.041 s
     with the JIT instead of 0.051 (0.039 without it).
3. [x] Warm start (`src/warm.cpp`), opt-in with `IXO_JIT_CACHE=1`:
   - At exit, each compiled function is saved with its loop entry points, keyed by a
     hash of its name and bytecode, so moved projects and bundled executables still
     match and edited functions are ignored.
   - The profile lives in `.ixo-cache/` at the project root, else the user's cache
     folder, one file per main file. Files are written privately and renamed into
     place, so concurrent runs never see half a profile. At most 256 profiles are kept,
     the least recently used go first. `ixo cache clean` empties the cache.
   - A known function compiles after 8 calls, its loops after 16 turns. Compiling at the
     very first call was tried and made closures, objects and format 8–9% slower: the
     code was generated before the inline caches had seen any types.
   - Measured: no gain on any workload (−3% to +2%, cold against warm, best of 7).
     That's why it is off by default. It is kept for Phase 12's tier 2, whose compiles
     cost more. `ixo build --train` waits for the same reason.
   - Verified: the JIT check agrees on all 155 programs on a cold and a warm run, and
     the test suite passes warm.
4. [ ] The compiler thread, with Phase 12's tier 2. Today's compiles take a median of
   157 µs and at most 1.1 ms (bench and examples), so the 0.1 ms target needs it, but
   only tier 2 makes it worth the synchronization: snapshots of inline caches, pinning
   and installation at safe points.
5. [ ] Promotion and demotion between tiers (with Phase 12), and freeing discarded code.

Targets:
- No pause over 0.1 ms on the program's thread for compiling.
- A warm second run of a short program (`bench/probes/manyfns`, the examples) at least
  20% faster than its first run.
- No program slower than today; never a different result.

## Phase 16: code less, do more (proposed, after Phases 10–15)

Goal: rework how Ixo is written and typed so that a program of a few hundred lines, a few
thousand words, is:
- **shorter than Python**;
- **as safe as Rust**: the mistakes Rust rejects are rejected or impossible;
- **close to C++ in speed** for the parts that matter;
- **more capable than Python across machines**: the same source on x86-64, ARM64, the
  browser and GPUs, using all cores.

This phase comes last on purpose. The speed it promises comes from Phases 10–15 (typed
containers, typed native code, beyond-C++ optimizations, GPU, automatic compilation), and
the language should be reshaped knowing what those need from it.

### How "code less, do more" is measured

Promises like this drift unless they are counted. Before any change, build the yardstick:

- **`bench/rosetta`: about 25 everyday tasks**, each written idiomatically in Ixo, Python,
  Rust and C++:
  - text processing: word frequencies, log parsing, templating;
  - data: CSV and JSON transforms, grouping, joins;
  - algorithms: graph search, dynamic programming, priority queues;
  - numbers: simulation (nbody), image filters, matrix code;
  - concurrency: an HTTP service, a crawler with a task pool, a pipeline over flows;
  - parallel work: map-reduce over cores, a GPU kernel (Phase 14);
  - modelling: classes, interfaces and enums for a small domain;
  - error-heavy I/O: reading files and settings with every failure handled;
  - a command-line tool with arguments and output formatting.
- **Measured for every task and language:**
  - lines, words and tokens (counted the same way for all four);
  - time and peak memory (`bench/langs` methods);
  - a safety sheet: which planted mistakes the language rejects before running. Mistakes
    planted: a missing nil check, an ignored error, a race on shared data, a use after
    free, an index out of range, a missing `match` case, a forgotten close.
- The suite runs in CI and its table is published in the docs, so every syntax proposal
  in this phase is judged by numbers, not taste.

### 1. Types without writing them

- **Inference across whole functions and modules.** Local types, return types, generic
  parameters and field types are inferred from use, not only from literals and
  annotations. Annotations become documentation, optional everywhere except on exported
  functions of a package, where they are part of the contract (and inferred ones can be
  written in by `ixo fmt --annotate`).
- **Strict and gradual modes:**
  - **Strict** (a setting in `ixo.toml`, or `$strict` at the top of a file): every
    expression has a static type, `any` must be written explicitly, and the safety rules
    below are errors. Strict code is what Phases 12–14 compile to typed machine code,
    SIMD and GPU, with no hints at all.
  - **Gradual** (today's behavior): stays the default for scripts and the REPL.
  - A strict module can use gradual ones through checked boundaries.
- **Numbers:** `int` and `float` stay; sized types (`i32`, `u8`, `f32`) exist for typed
  lists, GPU buffers and FFI, inferred where a value flows into them.

### 2. Data that says what it is

- **`data` declarations: immutable records in one line.**
  - Example: `data Point(x: float, y: float)`.
  - Equality, hashing, printing, copying with changes (`p.with(x = 3.0)`) and pattern
    matching come built in.
  - Being values, never shared mutably, they give Phase 13 its no-aliasing guarantee
    and Phase 14 its GPU structs. Classes stay for objects with identity and state.
- **Enums, shorter:** enums with fields exist today, in the indented form, and patterns
  name the enum (`Shape.Circle(r)`). Candidates to measure:
  - a one-line form for small enums: `enum Shape = Circle(r: float) | Rect(w: float, h: float)`;
  - variants written without the enum's name in patterns, when the checker knows the
    subject's type: `Circle(r) -> 3 * r * r`.

  Missing cases become errors in strict code (item 5).
- **Destructuring wherever a name is bound:** parameters (`fn dist((x1, y1), (x2, y2))`),
  loops (already), assignments, `match` arms.

### 3. Whole-collection operations

- **Numeric arrays with element-wise arithmetic:** `ys = xs * 2.0 + offsets`,
  `xs[xs > 0]`, `(a - b).abs.max`, with broadcasting, slicing and reductions. The same
  expression runs as SIMD on the CPU, across cores, or on the GPU (Phases 13–14), chosen
  by size and placement.
- **A decision to take first:** `[1, 2] * 3` repeats a list today. Arithmetic on lists
  would make that ambiguous. Options:
  - a separate numeric array type (`array[float]`, built from a typed list with
    `.array`): clear, one more type;
  - element-wise operators (`.*`, `.+`): no new type, more symbols.

  Rosetta's numeric tasks decide by word count and clarity.

### 4. Where code runs, in one word

- Today `fork` runs a call as a task, `par` on another core, and Phase 14 adds `gpu`. In
  this phase the three become one idea, placement:
  - a placement word before any call or block: `par compute(x)`,
    `gpu xs.map(fn(v) = v * v)`;
  - with a sensible default: big pure work on typed data may move to cores by itself
    (Phase 13).
- The same function compiles for every place it is used. The checker says plainly when a
  function can't run somewhere ("a gpu call can't use a str").

### 5. Safety at Rust's level, without Rust's words

What Rust guarantees, and how Ixo meets it in strict code:

| Rust guarantees                    | Ixo today                    | Ixo in strict code after this phase          |
|------------------------------------|------------------------------|----------------------------------------------|
| no use-after-free, no dangling data | yes: garbage collection       | same                                          |
| no data races                       | yes: `par` heaps are isolated; frozen data is shared read-only; `fork` tasks take turns | same, and checked for GPU buffers too |
| no null dereference                 | `T?` checked where types are known | every `nil` handled before use: a compile error otherwise |
| errors can't be ignored             | `T!` values; ignoring one is allowed | an unhandled `T!` is a compile error (handle with `?`, `!`, `match`, or `_ =` to ignore on purpose) |
| exhaustive `match`                  | warnings in some cases       | missing cases are errors for enums, `data` and types |
| bounds checked                      | yes (panic)                  | same; Phase 12 removes the checks it can prove |
| resources released                  | by the collector, eventually | `use f = fs.open(path)` closes at the end of the block, also on panic |
| integer overflow caught             | wraps silently               | to decide: checked by default with wrapping operators (`+%`), or wrapping kept and checked in `ixo test` |
| `unsafe` is explicit                | `ffi` calls are not marked   | raw memory (FFI, GPU addresses) only inside `unsafe`; `ixo check --unsafe` lists them |

Why this is shorter than Rust: a garbage collector removes ownership, borrowing and
lifetimes, which are most of Rust's annotations, while keeping its memory-safety result.
Isolated heaps give its thread safety without `Send`/`Sync`.

### 6. Fewer words, decided by the numbers

- After items 1–5, a syntax pass in the style of Phase 7: every form is measured in
  Rosetta words against Python's, and the ones that cost more are reworked.
- Candidates to evaluate, not decisions:
  - comprehension-like forms where `map`/`keep` chains read longer;
  - one-word imports of common modules;
  - pattern guards;
  - string and regex literals for common shapes (dates, paths);
  - a shorter `main` with typed command-line arguments parsed from the signature
    (`fn main(input: path, top: int = 10)`);
  - query-like operations on collections of records (group, join, sort by several
    keys).
- **Rule kept from Phase 7:** one way to write each thing; a shorter form replaces the
  longer one rather than adding a second spelling.

### 7. One source, every machine

- `ixo build --target linux-arm64 | macos-arm64 | windows-x64 | wasm` packs a program for
  another machine. The bytecode is portable; the runtime for each target is
  downloaded once and cached.
- Strict code runs as typed machine code on each CPU (Phases 12–13), on the GPU where
  asked (14), and starts warm from its profile (15).
- Every Rosetta task runs unchanged on x86-64, ARM64 and in the browser; the data-parallel
  ones also on a GPU, with the CPU fallback elsewhere.

### Migration

As in Phase 7:
- old spellings keep working with a warning and a quick fix;
- `ixo fmt --upgrade` rewrites them, and the language server offers the fix;
- examples, tests and docs move to the new forms;
- the next major version removes the old spellings.

This phase is the natural 1.0: the language stable, measured and documented.

### Steps

1. [ ] `bench/rosetta`: the tasks in four languages, the counting tool, the safety sheet,
   the table in CI and the docs. The baseline is recorded before any change.
2. [ ] Inference across functions and modules; strict and gradual modes; `ixo fmt
   --annotate`.
3. [ ] `data` records, enums with payloads, destructuring everywhere.
4. [ ] The safety rules of item 5, as errors in strict code, each with a quick fix.
5. [ ] The decision on numeric arrays, then whole-collection operations on SIMD, cores
   and GPU.
6. [ ] Placement words unified (`fork`, `par`, `gpu`).
7. [ ] The syntax pass of item 6: a proposal list with Rosetta numbers, decided with you,
   then implemented with deprecations.
8. [ ] Cross-target `ixo build`; Rosetta on every target.
9. [ ] Migration complete; docs rewritten around strict code; 1.0.

Targets (the motto, counted on `bench/rosetta`):
- **Shorter:** at most 0.8× Python's words on average, and more than Python on no task;
  at most half of Rust's or C++'s.
- **Fast:** strict Ixo within 1.5× idiomatic C++ on the numeric tasks, and faster than it
  on text, collection and service tasks.
- **Safe:** every planted mistake that Rust rejects before running is rejected by strict
  Ixo, or can't be written; `unsafe` appears only where it was written.
- **Everywhere:** every task runs unchanged on x86-64, ARM64 and wasm; the data-parallel
  ones also on a GPU.

## Evaluated: an LLVM backend (not planned)

`docs/llvm-backend.md` measures what generating machine code with LLVM would give
(`bench/backend/run.py`).
- On Ixo's dynamic values, LLVM's code is 1.3–4.6× faster than Ixo's JIT. With full
  types it is 4–10× faster.
- It costs 1000–2000× slower compiles (about 65 ms against 30 µs per function), a
  138 MB library, and harder deoptimization, GC, tasks and debugging.
- Most of the gain comes from types, not from LLVM.
- Next step instead: type specialization and register allocation in Ixo's own JIT.
  LLVM stays an option for a future ahead-of-time `ixo build --native`.

## Known limitations

- HTTPS on Linux and macOS uses the `curl` program (Windows uses WinHTTP); plain HTTP and the
  server need nothing.
- The JIT runs on x86-64 and ARM64; elsewhere (and in the browser playground) everything
  runs in the interpreter.
- `ixo fmt --upgrade` can't see two meaning changes: `//` used as a root (now integer
  division) and `for a, b in list` over pairs the checker doesn't know about (a loop like
  that says so once when it runs).
- In the playground (WebAssembly) there are no subprocesses, C libraries or sockets, and at
  most 3 `par` workers.
- Packages come from git repositories or folders; there's no central registry or version
  ranges (a dependency names a branch, tag or commit).
- `ixo build` packs modules, not the data files a program reads.
- Callbacks run from native code (sort comparators, `str` methods, operator methods) can't
  wait on flows or tasks. The collection helpers (`map`, `each`, ...) are written in Ixo, so
  their callbacks can.
- A `par` task's changes to module variables aren't seen by the caller (send results back
  through its return value or a flow). A panic in a `par` task stops the whole program.
- Renaming a field or method only finds uses whose receiver type the checker knows.
- Named arguments can't be used with `fork` / `par` calls or built-in functions.
- While debugging, `input()` returns `nil`, because stdin carries the debug protocol.
