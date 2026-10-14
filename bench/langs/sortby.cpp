// records sorted by a key (std::stable_sort with a lambda, the idiomatic way)
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

struct Person {
    std::string name;
    long long age;
};

static void run(long long n) {
    std::vector<Person> people;
    for (long long i = 0; i < n; i++) people.push_back({"p" + std::to_string(i), (i * 7919) % 100});
    std::vector<Person> by_age = people;
    std::stable_sort(by_age.begin(), by_age.end(), [](const Person& a, const Person& b) { return a.age < b.age; });
    std::printf("%lld %lld %s\n", by_age[0].age, by_age[n - 1].age, by_age[n / 2].name.c_str());
}

int main() { run(300000); }
