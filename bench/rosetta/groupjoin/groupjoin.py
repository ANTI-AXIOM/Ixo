# Employees joined with their departments, grouped by city: headcount and average salary.
from collections import defaultdict

cities = ["paris", "tokyo", "lima", "oslo"]
depts = [{"id": d, "city": cities[d * 3 % 4]} for d in range(40)]
staff = [{"name": f"e{i}", "dept": i * 7 % 40, "salary": 30_000 + i * 37 % 9000} for i in range(60_000)]
city_of = {d["id"]: d["city"] for d in depts}
groups = defaultdict(list)
for e in staff:
    groups[city_of[e["dept"]]].append(e)


def avg(people):
    return sum(e["salary"] for e in people) / len(people)


for city in sorted(groups, key=lambda c: -avg(groups[c])):
    print(city, len(groups[city]), f"{avg(groups[city]):.2f}")
