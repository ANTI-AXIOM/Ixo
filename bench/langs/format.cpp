// number to text formatting and parsing back
#include <cstdio>
#include <cstdlib>
#include <string>

static void run(long n) {
    long total = 0;
    double back = 0.0;
    char buf[64];
    for (long i = 0; i < n; i++) {
        int len = std::snprintf(buf, sizeof buf, "%ld:%.2f|%ld", i, i * 0.5, i % 13);
        std::string s(buf, len);
        total += (long)s.size();
        std::string head = s.substr(0, s.find('|'));
        back += std::strtod(head.substr(head.find(':') + 1).c_str(), nullptr);
    }
    std::printf("%ld %.1f\n", total, back);
}

int main() { run(300000); }
