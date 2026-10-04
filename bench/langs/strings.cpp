// string building, join, upper case, split, search
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

static void run(long n) {
    std::vector<std::string> words;
    for (long i = 0; i < n; i++) words.push_back("word" + std::to_string(i % 997) + "x");
    std::string text;
    for (size_t i = 0; i < words.size(); i++) {
        if (i) text += ' ';
        text += words[i];
    }
    for (char& c : text) c = (char)std::toupper((unsigned char)c);
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t sp = text.find(' ', start);
        parts.push_back(text.substr(start, sp == std::string::npos ? std::string::npos : sp - start));
        if (sp == std::string::npos) break;
        start = sp + 1;
    }
    long hits = 0, total = 0;
    for (auto& p : parts) hits += p.find('7') != std::string::npos;
    for (auto& p : parts) total += (long)p.size();
    std::string built;
    for (long i = 0; i < n / 10; i++) built += parts[i].substr(0, 2);
    std::printf("%zu %ld %ld %zu\n", parts.size(), hits, total, built.size());
}

int main() { run(300000); }
