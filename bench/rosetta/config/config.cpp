// Reading settings with every failure handled: a missing file, bad lines, bad numbers.
#include <charconv>
#include <cstdio>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct Settings {
    std::vector<std::pair<std::string, std::string>> values;
    std::vector<std::string> errors;
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::optional<Settings> load(const std::string& path) {
    static const std::set<std::string> numeric = {"port", "workers", "timeout"};
    std::ifstream in(path);
    if (!in) return std::nullopt;
    Settings s;
    std::string line;
    for (int i = 1; std::getline(in, line); i++) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            s.errors.push_back("line " + std::to_string(i) + ": expected key = value");
            continue;
        }
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (numeric.count(key)) {
            int n;
            auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
            if (ec != std::errc() || end != value.data() + value.size()) {
                s.errors.push_back("line " + std::to_string(i) + ": " + key + " is not a number: " + value);
                continue;
            }
            value = std::to_string(n);
        }
        s.values.push_back({key, value});
    }
    return s;
}

int main() {
    const std::string path = "settings.conf";
    {
        std::ofstream out(path);
        if (!out) return 1;
        out << "# server settings\nport = 8080\nhost = example.org\nworkers = many\ntimeout = 30\nbad line\n";
    }
    for (const std::string name : {"missing.conf", "settings.conf"}) {
        auto s = load(name);
        if (!s) {
            std::printf("%s: using defaults\n", name.c_str());
            continue;
        }
        for (auto& [k, v] : s->values) std::printf("%s %s\n", k.c_str(), v.c_str());
        for (auto& e : s->errors) std::printf("error: %s\n", e.c_str());
    }
    std::remove(path.c_str());
}
