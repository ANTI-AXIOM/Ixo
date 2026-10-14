// Registration of the native standard library.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "vm.hpp"

namespace ixo {

void registerAllBuiltins(VM& vm);
void registerCore(VM& vm);
void registerCollections(VM& vm);
void registerMath(VM& vm);
void registerIO(VM& vm);
void registerConcurrency(VM& vm);
void registerExt(VM& vm);
void registerNet(VM& vm);
void registerGpu(VM& vm);
void setScriptPath(VM& vm, const std::string& path);  // os.script / os.script_dir
struct Json;
std::string jsonEncode(VM& vm, Value v, int indent);  // json.str

// Writes to stdout (buffered); flushOut() before writing to stderr.
void writeOut(const std::string& s);
void flushOut();
// Debugger: send program output somewhere other than stdout, and keep input()
// away from stdin (which carries the debug protocol).
void setOutputHook(std::function<void(const std::string&)> hook);
extern bool gStdinDisabled;

// Runs a program with arguments (no shell). Returns false if it couldn't start.
bool runProcess(const std::vector<std::string>& argv, const std::string& input, std::string& output,
                int& exitCode);

// Stable merge sort that stays memory-safe with inconsistent comparators.
template <class Less>
void mergeSort(std::vector<Value>& v, Less less) {
    size_t n = v.size();
    if (n < 2) return;
    std::vector<Value> tmp(n);
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            size_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = less(v[j], v[i]) ? v[j++] : v[i++];
            while (i < mid) tmp[k++] = v[i++];
            while (j < hi) tmp[k++] = v[j++];
        }
        v.swap(tmp);
    }
}

}  // namespace ixo
