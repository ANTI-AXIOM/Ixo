// many small objects kept alive, each with a string and a short list
// (heap-allocated, as in Ixo and Python)
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct Rec {
    long id;
    std::string name;
    long score;
    std::vector<long> tags;
};

static void run(long n) {
    std::vector<std::unique_ptr<Rec>> recs;
    for (long i = 0; i < n; i++)
        recs.push_back(std::make_unique<Rec>(Rec{i, "name" + std::to_string(i), i % 100, {i % 7, i % 11}}));
    long total = 0, longNames = 0;
    for (auto& r : recs) {
        total += r->score + r->tags[0] + r->tags[1];
        if (r->name.size() > 9) longNames++;
    }
    std::printf("%zu %ld %ld\n", recs.size(), total, longNames);
}

int main() { run(300000); }
