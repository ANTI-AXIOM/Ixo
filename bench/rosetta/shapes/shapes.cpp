// A small domain model: shapes with an area and a perimeter, summarized.
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <vector>

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual double perimeter() const = 0;
};

struct Circle : Shape {
    double r;
    explicit Circle(double r) : r(r) {}
    double area() const override { return std::numbers::pi * r * r; }
    double perimeter() const override { return 2 * std::numbers::pi * r; }
};

struct Rect : Shape {
    double w, h;
    Rect(double w, double h) : w(w), h(h) {}
    double area() const override { return w * h; }
    double perimeter() const override { return 2 * (w + h); }
};

struct Triangle : Shape {
    double a, b, c;
    Triangle(double a, double b, double c) : a(a), b(b), c(c) {}
    double area() const override {
        double s = (a + b + c) / 2;
        return std::sqrt(s * (s - a) * (s - b) * (s - c));
    }
    double perimeter() const override { return a + b + c; }
};

int main() {
    std::vector<std::unique_ptr<Shape>> shapes;
    for (int i = 0; i < 30000; i++) {
        if (i % 3 == 0) shapes.push_back(std::make_unique<Circle>(i % 7 + 1));
        else if (i % 3 == 1) shapes.push_back(std::make_unique<Rect>(i % 5 + 1, i % 9 + 1));
        else shapes.push_back(std::make_unique<Triangle>(3, 4, i % 4 + 2));
    }
    double area = 0, perimeter = 0;
    int big = 0;
    for (auto& s : shapes) {
        area += s->area();
        perimeter += s->perimeter();
        big += s->area() > 50;
    }
    std::printf("%.3f %.3f %d\n", area, perimeter, big);
}
