# records sorted by a key (stable)
class Person:
    __slots__ = ("name", "age")

    def __init__(self, name, age):
        self.name = name
        self.age = age


def run(n):
    people = [Person(f"p{i}", (i * 7919) % 100) for i in range(n)]
    by_age = sorted(people, key=lambda p: p.age)
    print(by_age[0].age, by_age[n - 1].age, by_age[n // 2].name)


run(300_000)
