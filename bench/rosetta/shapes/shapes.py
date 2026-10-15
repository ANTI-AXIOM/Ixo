# A small domain model: shapes with an area and a perimeter, summarized.
import math
from dataclasses import dataclass


@dataclass
class Circle:
    r: float

    def area(self):
        return math.pi * self.r * self.r

    def perimeter(self):
        return 2 * math.pi * self.r


@dataclass
class Rect:
    w: float
    h: float

    def area(self):
        return self.w * self.h

    def perimeter(self):
        return 2 * (self.w + self.h)


@dataclass
class Triangle:
    a: float
    b: float
    c: float

    def area(self):
        s = (self.a + self.b + self.c) / 2
        return math.sqrt(s * (s - self.a) * (s - self.b) * (s - self.c))

    def perimeter(self):
        return self.a + self.b + self.c


def make(i):
    match i % 3:
        case 0:
            return Circle(i % 7 + 1)
        case 1:
            return Rect(i % 5 + 1, i % 9 + 1)
        case _:
            return Triangle(3, 4, i % 4 + 2)


shapes = [make(i) for i in range(30_000)]
print(f"{sum(s.area() for s in shapes):.3f}", f"{sum(s.perimeter() for s in shapes):.3f}", sum(s.area() > 50 for s in shapes))
