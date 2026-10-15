// Employees joined with their departments, grouped by city: headcount and average salary.
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct Dept {
    int id;
    std::string city;
};
struct Employee {
    std::string name;
    int dept, salary;
};

int main() {
    const char* cities[] = {"paris", "tokyo", "lima", "oslo"};
    std::vector<Dept> depts;
    for (int d = 0; d < 40; d++) depts.push_back({d, cities[d * 3 % 4]});
    std::vector<Employee> staff;
    for (int i = 0; i < 60000; i++) staff.push_back({"e" + std::to_string(i), i * 7 % 40, 30000 + i * 37 % 9000});
    std::unordered_map<int, std::string> city_of;
    for (auto& d : depts) city_of[d.id] = d.city;
    std::map<std::string, std::vector<const Employee*>> groups;
    for (auto& e : staff) groups[city_of[e.dept]].push_back(&e);
    auto avg = [](const std::vector<const Employee*>& people) {
        double sum = 0;
        for (auto* e : people) sum += e->salary;
        return sum / people.size();
    };
    std::vector<std::string> order;
    for (auto& [city, people] : groups) order.push_back(city);
    std::sort(order.begin(), order.end(), [&](auto& a, auto& b) { return avg(groups[a]) > avg(groups[b]); });
    for (auto& city : order) std::printf("%s %zu %.2f\n", city.c_str(), groups[city].size(), avg(groups[city]));
}
