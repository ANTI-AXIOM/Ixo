// The five most frequent words of a text.
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

int main() {
    std::istringstream in("the quick brown fox jumps over the lazy dog and the cat sat on the mat");
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);
    std::string text;
    for (int i = 0; i < 200000; i++) {
        if (i) text += ' ';
        text += words[i * 7 % words.size()];
    }
    std::unordered_map<std::string, int> counts;
    std::istringstream all(text);
    for (std::string w; all >> w;) counts[w]++;
    std::vector<std::pair<std::string, int>> top(counts.begin(), counts.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    for (size_t i = 0; i < 5 && i < top.size(); i++) std::cout << top[i].first << ' ' << top[i].second << '\n';
}
