// Requests per status code and the three slowest endpoints, from access log lines.
#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> paths = {"/", "/login", "/api/items", "/api/orders", "/static/app.js"};
    int codes[] = {200, 200, 404, 500, 302};
    std::vector<std::string> log;
    for (int i = 0; i < 50000; i++)
        log.push_back("10.0.0." + std::to_string(i % 50) + " GET " + paths[i % 5] + " " + std::to_string(codes[i * 3 % 5]) +
                      " " + std::to_string(i * 37 % 900) + "ms");
    std::map<std::string, int> byStatus;
    std::map<std::string, long> total, seen;
    for (auto& line : log) {
        std::istringstream in(line);
        std::string ip, method, path, status, ms;
        in >> ip >> method >> path >> status >> ms;
        byStatus[status]++;
        total[path] += std::stol(ms.substr(0, ms.size() - 2));
        seen[path]++;
    }
    for (auto& [s, n] : byStatus) std::printf("%s %d\n", s.c_str(), n);
    std::vector<std::string> keys;
    for (auto& [p, t] : total) keys.push_back(p);
    auto avg = [&](const std::string& p) { return (double)total[p] / seen[p]; };
    std::stable_sort(keys.begin(), keys.end(), [&](auto& a, auto& b) { return avg(a) > avg(b); });
    for (int i = 0; i < 3; i++) std::printf("%s %.1f\n", keys[i].c_str(), avg(keys[i]));
}
