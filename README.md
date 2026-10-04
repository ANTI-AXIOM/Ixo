# Ixo

Ixo is a small, readable programming language: indentation instead of braces and colons,
optional types that are checked before the program runs, errors as values, pattern matching,
interfaces and enums, lightweight tasks that talk through flows (channels), and `par` for
running work on every core.

```ixo
class Point(x, y)
    fn len = (self.x ** 2 + self.y ** 2).sqrt

points = [Point(3, 4), Point(1, 1), Point(6, 8)]
far = points.keep(_.len > 2).map(_.len)
print(far)                                  # [5.0, 10.0]

fn parse_port(text: str): int ! Error
    n = text.int ! _ -> return Error.BadPort(f"not a number: {text}")
    if n < 1 or n > 65535 -> return Error.BadPort(f"out of range: {n}")
    n

port = parse_port("80o") ! err
    print(err.msg)                          # not a number: 80o
    8080
```

## Install

From a release (once one is published):

```sh
curl -fsSL https://raw.githubusercontent.com/Eqixo/Ixo/main/install.sh | sh        # Linux, macOS
```

```powershell
irm https://raw.githubusercontent.com/Eqixo/Ixo/main/install.ps1 | iex             # Windows
```

Or build it; you need a C++20 compiler (g++ 11+ or clang 14+):

```sh
./build.sh                 # Linux / macOS / Git Bash   -> build/release/ixo
./build.ps1                # Windows PowerShell          -> build/release/ixo.exe
```

`playground/build.sh` builds the browser playground (WebAssembly, needs Emscripten); serve
`playground/dist/` over HTTP to use it.

## Quick start

```sh
ixo examples/hello.ixo     # run a program
ixo                        # interactive REPL
ixo check file.ixo         # report syntax and type errors without running
ixo fmt file.ixo           # format in place (ixo fmt --check to only report)
ixo test                   # run @test functions under tests/
ixo new app                # create a project (ixo.toml, main.ixo, lib/, tests/)
ixo add <git-url>          # add a package (ixo install / update / remove)
ixo build                  # pack the program into one executable
ixo --profile file.ixo     # run with the sampling profiler
ixo lsp                    # language server, used by editors
ixo dap                    # debug adapter, used by editors
```

For Visual Studio Code, install the extension in `editors/vscode` (see its README). It gives
highlighting, errors as you type, hover, go to definition, find references, rename,
signature help, completion, formatting and a debugger.

## Documentation

- [Language guide](docs/language.md): everything the language does, with examples
- [Standard library reference](docs/stdlib.md): generated with `ixo stdlib-docs`
- [Grammar](docs/grammar.md)
- [Roadmap and design decisions](ROADMAP.md)
- [examples/](examples/): small complete programs

## How it works

```
source ─ lexer ─ parser ─ resolver ─ type checker ─ compiler ─ bytecode VM
```

| file | job |
|---|---|
| `src/lexer.cpp` | tokens, including INDENT/DEDENT for significant indentation |
| `src/parser.cpp` | recursive-descent parser with error recovery; builds the AST |
| `src/resolver.cpp` | binds names to declarations, scopes, closures, class layouts, imports |
| `src/checker.cpp` | gradual type checker (inference + annotations) |
| `src/compiler.cpp` | AST to stack-machine bytecode |
| `src/vm.cpp`, `src/values.cpp` | the VM: calls, methods, operators, fibers (tasks), flows, regions, mark-and-sweep GC |
| `src/jit.cpp` | JIT for hot functions and loops (x86-64 and ARM64) |
| `src/par.cpp` | `par`: worker threads with their own heaps, value copying, shared flows, frozen values |
| `src/lib_*.cpp`, `src/prelude.cpp` | standard library (C++ and Ixo): `lib_ext` has re, time, csv, os.exec and ffi; `lib_net` HTTP |
| `src/tools.cpp`, `src/lineedit.cpp`, `src/lsp.cpp` | REPL and its line editor, formatter, test runner, language server |
| `src/profiler.cpp`, `src/dap.cpp` | sampling and memory profilers, debug adapter |
| `src/pkg.cpp`, `src/bundle.cpp`, `src/toml.cpp` | package manager, `ixo build`, ixo.toml reader |
| `src/process.cpp` | running other programs (watch mode, `os.exec`, git) |
| `playground/` | the browser playground (WebAssembly build, page, runner) |

## Tests

```sh
python tests/run.py                      # golden-output tests (tests/**/*.ixo)
python tests/tools/lsp_smoke.py          # language server end to end
python tests/tools/dap_smoke.py          # debugger end to end
python tests/tools/doc_check.py          # every example in docs/ compiles
python tests/tools/lsp_features.py       # completion, code actions, workspace features
python tests/tools/repl_keys.py          # the REPL's line editor, with real key presses
python tests/tools/jit_check.py          # same results with and without the JIT
python tests/tools/profile_check.py      # --profile, --memprofile, IXO_JIT_STATS
python tests/tools/pkg_check.py          # package manager (needs git)
python tests/tools/build_check.py        # ixo build
node tests/tools/playground_check.mjs    # the playground in headless Chrome
python tests/run.py --ixo build/wasm/ixo --platform wasm   # the suite on WebAssembly
python bench/run.py                      # benchmarks
python bench/langs/run.py --no-jit       # Ixo vs C++ (g++ -O2) vs Python: time and peak memory
python3 bench/backend/run.py             # Ixo's JIT vs LLVM-compiled code (Linux/macOS, clang)
python bench/probes/run.py               # runtime-efficiency probes (ROADMAP Phase 11)
python tests/tools/fuzz.py 500           # parser/checker/formatter robustness
python tests/tools/fuzz_run.py 500       # VM robustness
IXO_GC_STRESS=1 python tests/run.py      # collect garbage at every safe point
```

GitHub Actions runs these on Linux, Windows and macOS (`.github/workflows/ci.yml`). Pushing a
`v*` tag builds release binaries and the VS Code extension into a draft release
(`release.yml`); `playground.yml` builds the playground and can publish it to GitHub Pages.

A test is an `.ixo` file with its expected output in comments:

```ixo
print(1 + 2)        # expect: 3
print(1 / 0)        # expect panic: division by zero
```
