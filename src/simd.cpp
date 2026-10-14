// Whole-list kernels for typed lists, one version per kind of CPU (see simd.hpp).
#include "simd.hpp"

#include <cstdlib>

#if defined(__x86_64__) || defined(_M_X64)
#define IXO_SIMD_X64 1
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define IXO_SIMD_NEON 1
#include <arm_neon.h>
#endif

namespace ixo {

namespace {

CpuInfo detect() {
    CpuInfo c;
#if defined(IXO_SIMD_X64)
    c.arch = "x86-64";
#if defined(__GNUC__)
    __builtin_cpu_init();
    c.sse42 = __builtin_cpu_supports("sse4.2");
    c.avx2 = __builtin_cpu_supports("avx2");
    c.fma = __builtin_cpu_supports("fma");
    c.avx512 = __builtin_cpu_supports("avx512f");
#endif
#elif defined(IXO_SIMD_NEON)
    c.arch = "arm64";
    c.neon = true;  // every AArch64 CPU has it
#elif defined(__wasm__)
    c.arch = "wasm32";
#else
    c.arch = "other";
#endif
    if (const char* e = std::getenv("IXO_SIMD")) {  // IXO_SIMD=none / sse4.2: narrower versions (for tests)
        if (std::string(e) == "none") c.sse42 = c.avx2 = c.avx512 = c.neon = false;
        if (std::string(e) == "sse4.2") c.avx2 = c.avx512 = false;
    }
    return c;
}

// ---- plain versions (also the reference the others must match)
int64_t sumIntsPlain(const int64_t* p, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; i++) s += (uint64_t)p[i];
    return (int64_t)s;
}
int64_t minIntsPlain(const int64_t* p, size_t n) {
    int64_t m = p[0];
    for (size_t i = 1; i < n; i++)
        if (p[i] < m) m = p[i];
    return m;
}
int64_t maxIntsPlain(const int64_t* p, size_t n) {
    int64_t m = p[0];
    for (size_t i = 1; i < n; i++)
        if (p[i] > m) m = p[i];
    return m;
}
size_t findIntPlain(const int64_t* p, size_t n, int64_t x) {
    for (size_t i = 0; i < n; i++)
        if (p[i] == x) return i;
    return n;
}
double sumFloatsFastPlain(const double* p, size_t n) {
    double a = 0, b = 0, c = 0, d = 0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        a += p[i];
        b += p[i + 1];
        c += p[i + 2];
        d += p[i + 3];
    }
    for (; i < n; i++) a += p[i];
    return (a + b) + (c + d);
}

#if defined(IXO_SIMD_X64) && defined(__GNUC__)
__attribute__((target("avx2"))) int64_t sumIntsAvx2(const int64_t* p, size_t n) {
    __m256i a = _mm256_setzero_si256(), b = _mm256_setzero_si256();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        a = _mm256_add_epi64(a, _mm256_loadu_si256((const __m256i*)(p + i)));
        b = _mm256_add_epi64(b, _mm256_loadu_si256((const __m256i*)(p + i + 4)));
    }
    a = _mm256_add_epi64(a, b);
    alignas(32) int64_t t[4];
    _mm256_store_si256((__m256i*)t, a);
    uint64_t s = (uint64_t)t[0] + (uint64_t)t[1] + (uint64_t)t[2] + (uint64_t)t[3];
    for (; i < n; i++) s += (uint64_t)p[i];
    return (int64_t)s;
}
int64_t sumIntsSse2(const int64_t* p, size_t n) {
    __m128i a = _mm_setzero_si128(), b = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        a = _mm_add_epi64(a, _mm_loadu_si128((const __m128i*)(p + i)));
        b = _mm_add_epi64(b, _mm_loadu_si128((const __m128i*)(p + i + 2)));
    }
    a = _mm_add_epi64(a, b);
    alignas(16) int64_t t[2];
    _mm_store_si128((__m128i*)t, a);
    uint64_t s = (uint64_t)t[0] + (uint64_t)t[1];
    for (; i < n; i++) s += (uint64_t)p[i];
    return (int64_t)s;
}
template <bool Max>
__attribute__((target("avx2"))) int64_t minMaxAvx2(const int64_t* p, size_t n) {
    if (n < 8) return Max ? maxIntsPlain(p, n) : minIntsPlain(p, n);
    __m256i m = _mm256_loadu_si256((const __m256i*)p);
    size_t i = 4;
    for (; i + 4 <= n; i += 4) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(p + i));
        __m256i gt = Max ? _mm256_cmpgt_epi64(v, m) : _mm256_cmpgt_epi64(m, v);
        m = _mm256_blendv_epi8(m, v, gt);
    }
    alignas(32) int64_t t[4];
    _mm256_store_si256((__m256i*)t, m);
    int64_t r = t[0];
    for (int k = 1; k < 4; k++) r = Max ? (t[k] > r ? t[k] : r) : (t[k] < r ? t[k] : r);
    for (; i < n; i++) r = Max ? (p[i] > r ? p[i] : r) : (p[i] < r ? p[i] : r);
    return r;
}
template <bool Max>
__attribute__((target("sse4.2"))) int64_t minMaxSse42(const int64_t* p, size_t n) {
    if (n < 4) return Max ? maxIntsPlain(p, n) : minIntsPlain(p, n);
    __m128i m = _mm_loadu_si128((const __m128i*)p);
    size_t i = 2;
    for (; i + 2 <= n; i += 2) {
        __m128i v = _mm_loadu_si128((const __m128i*)(p + i));
        __m128i gt = Max ? _mm_cmpgt_epi64(v, m) : _mm_cmpgt_epi64(m, v);
        m = _mm_blendv_epi8(m, v, gt);
    }
    alignas(16) int64_t t[2];
    _mm_store_si128((__m128i*)t, m);
    int64_t r = Max ? (t[0] > t[1] ? t[0] : t[1]) : (t[0] < t[1] ? t[0] : t[1]);
    for (; i < n; i++) r = Max ? (p[i] > r ? p[i] : r) : (p[i] < r ? p[i] : r);
    return r;
}
__attribute__((target("avx2"))) size_t findIntAvx2(const int64_t* p, size_t n, int64_t x) {
    __m256i want = _mm256_set1_epi64x(x);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        int mask = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(_mm256_loadu_si256((const __m256i*)(p + i)), want)));
        if (mask) return i + (size_t)__builtin_ctz((unsigned)mask);
    }
    for (; i < n; i++)
        if (p[i] == x) return i;
    return n;
}
__attribute__((target("avx2"))) double sumFloatsFastAvx2(const double* p, size_t n) {
    __m256d a = _mm256_setzero_pd(), b = _mm256_setzero_pd(), c = _mm256_setzero_pd(), d = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        a = _mm256_add_pd(a, _mm256_loadu_pd(p + i));
        b = _mm256_add_pd(b, _mm256_loadu_pd(p + i + 4));
        c = _mm256_add_pd(c, _mm256_loadu_pd(p + i + 8));
        d = _mm256_add_pd(d, _mm256_loadu_pd(p + i + 12));
    }
    a = _mm256_add_pd(_mm256_add_pd(a, b), _mm256_add_pd(c, d));
    alignas(32) double t[4];
    _mm256_store_pd(t, a);
    double s = (t[0] + t[1]) + (t[2] + t[3]);
    for (; i < n; i++) s += p[i];
    return s;
}
#endif

#if defined(IXO_SIMD_NEON)
int64_t sumIntsNeon(const int64_t* p, size_t n) {
    int64x2_t a = vdupq_n_s64(0), b = vdupq_n_s64(0);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        a = vaddq_s64(a, vld1q_s64(p + i));
        b = vaddq_s64(b, vld1q_s64(p + i + 2));
    }
    a = vaddq_s64(a, b);
    uint64_t s = (uint64_t)vgetq_lane_s64(a, 0) + (uint64_t)vgetq_lane_s64(a, 1);
    for (; i < n; i++) s += (uint64_t)p[i];
    return (int64_t)s;
}
template <bool Max>
int64_t minMaxNeon(const int64_t* p, size_t n) {
    if (n < 4) return Max ? maxIntsPlain(p, n) : minIntsPlain(p, n);
    int64x2_t m = vld1q_s64(p);
    size_t i = 2;
    for (; i + 2 <= n; i += 2) {
        int64x2_t v = vld1q_s64(p + i);
        uint64x2_t gt = Max ? vcgtq_s64(v, m) : vcgtq_s64(m, v);
        m = vbslq_s64(gt, v, m);
    }
    int64_t x = vgetq_lane_s64(m, 0), y = vgetq_lane_s64(m, 1);
    int64_t r = Max ? (x > y ? x : y) : (x < y ? x : y);
    for (; i < n; i++) r = Max ? (p[i] > r ? p[i] : r) : (p[i] < r ? p[i] : r);
    return r;
}
double sumFloatsFastNeon(const double* p, size_t n) {
    float64x2_t a = vdupq_n_f64(0), b = vdupq_n_f64(0), c = vdupq_n_f64(0), d = vdupq_n_f64(0);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        a = vaddq_f64(a, vld1q_f64(p + i));
        b = vaddq_f64(b, vld1q_f64(p + i + 2));
        c = vaddq_f64(c, vld1q_f64(p + i + 4));
        d = vaddq_f64(d, vld1q_f64(p + i + 6));
    }
    a = vaddq_f64(vaddq_f64(a, b), vaddq_f64(c, d));
    double s = vgetq_lane_f64(a, 0) + vgetq_lane_f64(a, 1);
    for (; i < n; i++) s += p[i];
    return s;
}
#endif

}  // namespace

const CpuInfo& cpuInfo() {
    static const CpuInfo c = detect();
    return c;
}

std::string CpuInfo::simd() const {
    return avx512 && avx2 ? "avx2 (avx-512 present)" : avx2 ? "avx2" : sse42 ? "sse4.2" : neon ? "neon" : "none";
}

int64_t simdSumInts(const int64_t* p, size_t n) {
#if defined(IXO_SIMD_X64) && defined(__GNUC__)
    if (cpuInfo().avx2) return sumIntsAvx2(p, n);
    if (cpuInfo().sse42) return sumIntsSse2(p, n);
#elif defined(IXO_SIMD_NEON)
    if (cpuInfo().neon) return sumIntsNeon(p, n);
#endif
    return sumIntsPlain(p, n);
}

double simdSumFloats(const double* p, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s;
}

double simdSumFloatsFast(const double* p, size_t n) {
#if defined(IXO_SIMD_X64) && defined(__GNUC__)
    if (cpuInfo().avx2) return sumFloatsFastAvx2(p, n);
#elif defined(IXO_SIMD_NEON)
    if (cpuInfo().neon) return sumFloatsFastNeon(p, n);
#endif
    return sumFloatsFastPlain(p, n);
}

int64_t simdMinInts(const int64_t* p, size_t n) {
#if defined(IXO_SIMD_X64) && defined(__GNUC__)
    if (cpuInfo().avx2) return minMaxAvx2<false>(p, n);
    if (cpuInfo().sse42) return minMaxSse42<false>(p, n);
#elif defined(IXO_SIMD_NEON)
    if (cpuInfo().neon) return minMaxNeon<false>(p, n);
#endif
    return minIntsPlain(p, n);
}

int64_t simdMaxInts(const int64_t* p, size_t n) {
#if defined(IXO_SIMD_X64) && defined(__GNUC__)
    if (cpuInfo().avx2) return minMaxAvx2<true>(p, n);
    if (cpuInfo().sse42) return minMaxSse42<true>(p, n);
#elif defined(IXO_SIMD_NEON)
    if (cpuInfo().neon) return minMaxNeon<true>(p, n);
#endif
    return maxIntsPlain(p, n);
}

size_t simdFindInt(const int64_t* p, size_t n, int64_t x) {
#if defined(IXO_SIMD_X64) && defined(__GNUC__)
    if (cpuInfo().avx2) return findIntAvx2(p, n, x);
#endif
    return findIntPlain(p, n, x);
}

size_t simdFindFloat(const double* p, size_t n, double x) {
    for (size_t i = 0; i < n; i++)
        if (p[i] == x) return i;
    return n;
}

size_t simdCountTrue(const uint8_t* p, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) c += p[i] != 0;
    return c;
}

}  // namespace ixo
