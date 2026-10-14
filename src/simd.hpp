// Whole-list operations on typed lists (Phase 10's ints, floats and bools),
// written for the CPU the program runs on: the widest SIMD it has is picked
// once at start-up (AVX2 or SSE4.2 on x86-64, NEON on ARM64), with a plain
// loop everywhere else. Results never depend on which version runs: int sums
// wrap the same in any order, float sums keep the program's order unless asked
// for `sum_fast` (which may change the last bits).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ixo {

struct CpuInfo {
    std::string arch;  // "x86-64", "arm64", "wasm32", ...
    bool sse42 = false, avx2 = false, fma = false, avx512 = false, neon = false;
    std::string simd() const;  // the widest in use: "avx2", "sse4.2", "neon" or "none"
};
const CpuInfo& cpuInfo();

int64_t simdSumInts(const int64_t* p, size_t n);
double simdSumFloats(const double* p, size_t n);      // in order: exactly a loop's result
double simdSumFloatsFast(const double* p, size_t n);  // reordered (several partial sums)
int64_t simdMinInts(const int64_t* p, size_t n);      // n > 0
int64_t simdMaxInts(const int64_t* p, size_t n);      // n > 0
size_t simdFindInt(const int64_t* p, size_t n, int64_t x);  // n when not found
size_t simdFindFloat(const double* p, size_t n, double x);  // == on doubles (NaN: never found)
size_t simdCountTrue(const uint8_t* p, size_t n);

}  // namespace ixo
