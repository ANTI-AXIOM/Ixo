# Rosetta: how much Ixo it takes

`bench/rosetta` holds 16 everyday tasks, each written idiomatically in Ixo, Python, C++ and
Rust, printing the same output (`expected.txt`). Phase 16 of the roadmap ("code less, do
more") is measured on them.

```text
python bench/rosetta/run.py            # counts, runs, checks, prints the tables
python bench/rosetta/run.py --md       # the tables as Markdown (this page)
python bench/rosetta/run.py --safety   # the safety sheet
```

## How it counts

- **Words** are what's left after comments and blank lines are removed, split on
  whitespace. **Lines** are non-blank lines of code. **Tokens** count a string literal as
  one, an operator like `->` or `+=` as one, and every name, number and other character as
  one. The rules are the same for all four languages.
- **Times** are the best of 3 runs, start-up included; **memory** is the peak. C++ is built
  with `g++ -O2 -std=c++20` (statically on Windows); Rust isn't run where `rustc` isn't
  installed (it is still counted).
- Versions that need a library the machine lacks are counted but not run: CuPy for the GPU
  task in Python, CUDA for it in C++, nlohmann/json, and crates (serde_json, clap, cudarc).

## The tasks

| task | what it does |
|---|---|
| wordfreq | the five most frequent words of a text |
| logparse | requests per status code and the slowest endpoints, from log lines |
| csvreport | totals and the best product per region, from CSV text |
| jsonreshape | orders as JSON to a per-customer summary as JSON |
| groupjoin | employees joined with departments, grouped by city |
| bfs | shortest path through a grid maze |
| dijkstra | shortest distances in a weighted graph, with a binary heap |
| lcs | longest common subsequence (dynamic programming) |
| nbody | the n-body simulation of the Jovian planets |
| blur | a 3x3 box blur over an image |
| pipeline | a producer, four workers and a consumer over channels |
| mapreduce | counting primes on every core |
| gpu_saxpy | a GPU kernel over a million floats |
| shapes | a small domain model: shapes with areas and perimeters |
| config | reading settings with every failure handled |
| cli | a command-line tool: arguments, counting, a formatted table |

Twenty-five tasks were planned; these 16 cover every kind the roadmap lists. Templating
and matrix code are left to `bench/gpu` (matmul) and `bench/langs`. An HTTP service and a
crawler would measure the network rather than the language, so `pipeline` covers tasks
working together.

## Words: before and after Phase 16

The baseline was recorded before any Phase 16 change. "Now" uses the forms Phase 16 added
where a Python programmer would reach for the equivalent: `.counts`, `m[k] += v` from
nothing, `_` in unpacking, chained comparisons, `fn main` with typed command-line
parameters, one-line enums.

| task | Ixo before | Ixo now | Python | C++ | Rust | Ixo/Python before | now |
|---|---:|---:|---:|---:|---:|---:|---:|
| bfs | 105 | 92 | 95 | 158 | 152 | 1.11 | 0.97 |
| blur | 67 | 67 | 80 | 164 | 101 | 0.84 | 0.84 |
| cli | 93 | 39 | 54 | 193 | 91 | 1.72 | 0.72 |
| config | 128 | 128 | 131 | 253 | 181 | 0.98 | 0.98 |
| csvreport | 48 | 48 | 75 | 177 | 137 | 0.64 | 0.64 |
| dijkstra | 89 | 89 | 91 | 152 | 126 | 0.98 | 0.98 |
| gpu_saxpy | 43 | 38 | 40 | 124 | 113 | 1.07 | 0.95 |
| groupjoin | 55 | 55 | 82 | 162 | 134 | 0.67 | 0.67 |
| jsonreshape | 55 | 55 | 69 | 117 | 92 | 0.80 | 0.80 |
| lcs | 73 | 71 | 70 | 107 | 96 | 1.04 | 1.01 |
| logparse | 86 | 77 | 89 | 173 | 132 | 0.97 | 0.87 |
| mapreduce | 55 | 55 | 74 | 103 | 81 | 0.74 | 0.74 |
| nbody | 290 | 290 | 315 | 406 | 390 | 0.92 | 0.92 |
| pipeline | 62 | 62 | 70 | 169 | 73 | 0.89 | 0.89 |
| shapes | 121 | 124 | 151 | 258 | 178 | 0.80 | 0.82 |
| wordfreq | 49 | 35 | 48 | 129 | 74 | 1.02 | 0.73 |
| **total** | **1419** | **1325** | **1534** | **2845** | **2151** | | |

Ixo's words against each language's, now (mean of the per-task ratios):

| | mean | most |
|---|---:|---:|
| Ixo / Python | 0.85 (was 0.95) | 1.01, lcs (was 1.72, cli) |
| Ixo / Rust | 0.60 | 0.85, pipeline |
| Ixo / C++ | 0.45 | 0.71, nbody |

In tokens Ixo is at 2782 against Python's 3101, Rust's 5468 and C++'s 6452.

## Time and memory

Windows x86-64; best of 3 runs, start-up included.

| task | Ixo | Python | C++ |
|---|---|---|---|
| bfs | 0.41 s, 34 MB | 0.42 s, 33 MB | 0.01 s, 4 MB |
| blur | 0.32 s, 10 MB | 0.40 s, 11 MB | 0.01 s, 4 MB |
| cli | 0.01 s, 7 MB | 0.07 s, 16 MB | 0.01 s, 4 MB |
| config | 0.01 s, 7 MB | 0.03 s, 10 MB | 0.01 s, 4 MB |
| csvreport | 0.06 s, 27 MB | 0.09 s, 21 MB | 0.03 s, 7 MB |
| dijkstra | 0.06 s, 18 MB | 0.08 s, 20 MB | 0.01 s, 5 MB |
| gpu_saxpy | 0.28 s, 60 MB | (needs CuPy) | (needs CUDA) |
| groupjoin | 0.06 s, 25 MB | 0.10 s, 28 MB | 0.01 s, 7 MB |
| jsonreshape | 0.03 s, 18 MB | 0.07 s, 22 MB | (needs nlohmann/json) |
| lcs | 0.41 s, 13 MB | 0.59 s, 11 MB | 0.01 s, 4 MB |
| logparse | 0.11 s, 21 MB | 0.09 s, 15 MB | 0.05 s, 9 MB |
| mapreduce | 0.16 s, 12 MB | 0.69 s, 20 MB | 0.01 s, 4 MB |
| nbody | 0.17 s, 12 MB | 0.38 s, 14 MB | 0.02 s, 4 MB |
| pipeline | 0.02 s, 8 MB | 0.33 s, 11 MB | 0.09 s, 4 MB |
| shapes | 0.03 s, 10 MB | 0.09 s, 17 MB | 0.01 s, 5 MB |
| wordfreq | 0.05 s, 28 MB | 0.07 s, 24 MB | 0.01 s, 6 MB |

## Every machine

Every task runs unchanged and prints `expected.txt` on Windows x86-64, Linux x86-64, Linux
ARM64 (qemu) and WebAssembly (Node). The Linux and ARM64 runs used executables built on
Windows with `ixo build --target`. `gpu_saxpy` runs on a GPU through Vulkan (an Intel UHD
on Windows, llvmpipe on Linux), and through WebGPU in the playground.

## The safety sheet

Mistakes planted in each language (`bench/rosetta/safety/*.ixo` for Ixo, which `--safety`
checks with `ixo check`), and what each language does before the program runs:

| planted mistake | strict Ixo | Python | Rust | C++ |
|---|---|---|---|---|
| a missing nil check | rejected | runtime | rejected | runtime |
| an ignored error | rejected | runtime | warning | runtime |
| a race on shared data | rejected | runtime | rejected | runtime |
| a use after free | impossible | impossible | rejected | runtime |
| an index out of range | rejected | runtime | rejected | warning |
| a missing match case | rejected | runtime | rejected | warning |
| a forgotten close | rejected | warning | impossible | impossible |

- Python: `nil` is None (an AttributeError when used); an ignored error is an exception
  nobody catches; a race is a lost update between threads; a forgotten close is a
  ResourceWarning at best.
- Rust: an ignored `Result` is the `unused_must_use` warning. An index out of range is
  rejected for arrays indexed by constants, and panics at run time otherwise.
- C++: `-Wall` warns about a missing enum case and some constant out-of-range indexes;
  RAII closes files.
- Ixo's index case is a fixed-size tuple indexed by a constant, which is rejected in every
  mode. Lists are checked when the program runs, as in every language here.

## The syntax pass

Each candidate from the roadmap, measured in Rosetta words and tokens, with the decision:

| candidate | what it changed | decision |
|---|---|---|
| typed command-line parameters (`fn main(input, top = 10)`) | cli 93 → 39 words | adopted |
| `xs.counts` | wordfreq 49 → 35 (with the unpacking below), cli | adopted |
| `m[k] += v` starting from nothing | logparse 86 → 77 (with `_`) | adopted |
| `_` in unpacking | logparse | adopted |
| chained comparisons | bfs 105 → 92 (with iterating a growing list) | adopted |
| one-line enums, variants without the enum's name | shapes: 3 words more, 9 tokens fewer | adopted (fewer tokens, clearer patterns) |
| comprehension forms | `map`/`keep` with `_` are already shorter than Python's comprehensions | rejected |
| one-word imports | the standard modules are already there without `use` | not needed |
| pattern guards | already in the language | not needed |
| literals for dates and paths | no task needs them | rejected |
| query operations (group, join, sort by keys) | `group_by`, `sort_by((a, b))` and maps already cover groupjoin (0.67) | rejected |
| empty collections false (`while heap`) | dijkstra, 3 words | rejected: it changes what existing programs mean |
| `x if c else y` | lcs, 1 word | rejected: one way to write an `if` |

The rule kept from Phase 7 holds: each new form replaced a longer way of writing the same
thing; nothing became a second spelling.

## Targets

The roadmap's targets, as measured here:

- **Shorter**: at most 0.8× Python's words on average and never more than Python. **Not
  met**: 0.85 on average, and lcs is at 1.01 (one word, the `->` of Ixo's
  `if a -> b else c`). Against Rust and C++, at most half: met for C++ (0.45), not for
  Rust (0.60).
- **Fast**: strict Ixo within 1.5× idiomatic C++ on the numeric tasks. **Not met**: nbody
  is about 8×, lcs and blur more. Faster than C++ on text, collection and service tasks:
  **not met** either. Ixo is faster than Python on 14 of the 15 tasks Python runs (logparse
  is the exception). C++ is faster on all but `pipeline`, and ties on the shortest ones.
- **Safe**: every planted mistake that Rust rejects is rejected by strict Ixo, or can't be
  written. **Met.**
- **Everywhere**: every task runs unchanged on x86-64, ARM64 and wasm, and the data-parallel
  one on a GPU. **Met.**
