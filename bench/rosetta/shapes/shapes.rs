// A small domain model: shapes with an area and a perimeter, summarized.
use std::f64::consts::PI;

enum Shape {
    Circle(f64),
    Rect(f64, f64),
    Triangle(f64, f64, f64),
}

impl Shape {
    fn area(&self) -> f64 {
        match *self {
            Shape::Circle(r) => PI * r * r,
            Shape::Rect(w, h) => w * h,
            Shape::Triangle(a, b, c) => {
                let s = (a + b + c) / 2.0;
                (s * (s - a) * (s - b) * (s - c)).sqrt()
            }
        }
    }
    fn perimeter(&self) -> f64 {
        match *self {
            Shape::Circle(r) => 2.0 * PI * r,
            Shape::Rect(w, h) => 2.0 * (w + h),
            Shape::Triangle(a, b, c) => a + b + c,
        }
    }
}

fn main() {
    let shapes: Vec<Shape> = (0..30_000)
        .map(|i| match i % 3 {
            0 => Shape::Circle((i % 7 + 1) as f64),
            1 => Shape::Rect((i % 5 + 1) as f64, (i % 9 + 1) as f64),
            _ => Shape::Triangle(3.0, 4.0, (i % 4 + 2) as f64),
        })
        .collect();
    let area: f64 = shapes.iter().map(Shape::area).sum();
    let perimeter: f64 = shapes.iter().map(Shape::perimeter).sum();
    let big = shapes.iter().filter(|s| s.area() > 50.0).count();
    println!("{:.3} {:.3} {}", area, perimeter, big);
}
