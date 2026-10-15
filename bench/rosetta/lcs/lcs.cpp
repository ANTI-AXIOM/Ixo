// Length of the longest common subsequence of two DNA strings (dynamic programming).
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

std::string dna(long long x, int n) {
    std::string out;
    for (int i = 0; i < n; i++) {
        x = (x * 1103515245 + 12345) % 2147483648LL;
        out += "ACGT"[(x >> 16) & 3];
    }
    return out;
}

int main() {
    std::string a = dna(1, 1500), b = dna(2, 1500);
    std::vector<int> prev(b.size() + 1, 0), row(b.size() + 1);
    for (char ca : a) {
        row[0] = 0;
        for (size_t j = 0; j < b.size(); j++) row[j + 1] = ca == b[j] ? prev[j] + 1 : std::max(prev[j + 1], row[j]);
        std::swap(prev, row);
    }
    std::printf("%d\n", prev.back());
}
