// A command-line tool: count the words given as arguments and print the most common.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

[[noreturn]] void usage(const char* msg) {
    std::fprintf(stderr, "error: %s\nusage: cli [--top N] [--upper] words...\n", msg);
    std::exit(2);
}

int main(int argc, char** argv) {
    size_t top = 5;
    bool upper = false;
    std::vector<std::string> words;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--top") {
            if (++i >= argc) usage("--top needs a number");
            char* end;
            long n = std::strtol(argv[i], &end, 10);
            if (*end || n < 0) usage("--top needs a number");
            top = n;
        } else if (a == "--upper") {
            upper = true;
        } else {
            words.push_back(a);
        }
    }
    if (words.empty()) usage("no words given");
    std::vector<std::pair<std::string, int>> counts;  // first-seen order
    size_t width = 0;
    for (auto w : words) {
        if (upper) std::transform(w.begin(), w.end(), w.begin(), ::toupper);
        auto it = std::find_if(counts.begin(), counts.end(), [&](auto& c) { return c.first == w; });
        if (it != counts.end()) it->second++;
        else counts.push_back({w, 1});
        width = std::max(width, w.size());
    }
    std::stable_sort(counts.begin(), counts.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < counts.size() && i < top; i++)
        std::printf("%-*s %3d %s\n", (int)width, counts[i].first.c_str(), counts[i].second, std::string(counts[i].second, '#').c_str());
}
