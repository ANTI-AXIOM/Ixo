// Sales by region from CSV text: totals, the best product of each region, sorted.
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct Row {
    std::string region, product;
    int units;
    double price;
};

int main() {
    const char* names[] = {"north", "south", "east", "west"};
    std::string text = "region,product,units,price\n";
    char buf[128];
    for (int i = 0; i < 20000; i++) {
        std::snprintf(buf, sizeof buf, "%s,p%d,%d,%g\n", names[i % 4], i % 13, i % 7 + 1, i % 11 + 0.5);
        text += buf;
    }
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);  // header
    std::map<std::string, std::vector<Row>> regions;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string f[4];
        for (auto& x : f) std::getline(ss, x, ',');
        regions[f[0]].push_back({f[0], f[1], std::stoi(f[2]), std::stod(f[3])});
    }
    for (auto& [region, sales] : regions) {
        double total = 0;
        std::vector<std::pair<std::string, int>> units;  // first-seen order
        for (auto& r : sales) {
            total += r.units * r.price;
            bool found = false;
            for (auto& u : units)
                if (u.first == r.product) {
                    u.second += r.units;
                    found = true;
                }
            if (!found) units.push_back({r.product, r.units});
        }
        auto best = units[0];
        for (auto& u : units)
            if (u.second > best.second) best = u;
        std::printf("%s %.2f %s\n", region.c_str(), total, best.first.c_str());
    }
}
