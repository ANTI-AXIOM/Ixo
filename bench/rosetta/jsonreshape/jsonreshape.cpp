// Orders as JSON to a per-customer summary as JSON (with nlohmann/json).
#include <cstdio>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
using nlohmann::json;

int main() {
    json orders = json::array();
    for (int i = 0; i < 5000; i++) {
        json items = json::array();
        for (int j = 0; j < i % 4 + 1; j++) items.push_back({{"sku", "s" + std::to_string(j)}, {"qty", j + 1}});
        orders.push_back({{"id", i}, {"customer", "c" + std::to_string(i % 37)}, {"items", items}});
    }
    std::string text = orders.dump();
    std::map<std::string, std::pair<int, int>> summary;
    for (auto& o : json::parse(text)) {
        auto& s = summary[o["customer"].get<std::string>()];
        s.first++;
        for (auto& item : o["items"]) s.second += item["qty"].get<int>();
    }
    json out = json::array();
    for (auto& [c, s] : summary) out.push_back({{"customer", c}, {"orders", s.first}, {"units", s.second}});
    std::string str = out.dump();
    std::printf("%zu %s\n", str.size(), str.substr(0, 120).c_str());
}
