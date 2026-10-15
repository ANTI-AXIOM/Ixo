// Employees joined with their departments, grouped by city: headcount and average salary.
use std::collections::HashMap;

struct Dept {
    id: usize,
    city: &'static str,
}
struct Employee {
    #[allow(dead_code)]
    name: String,
    dept: usize,
    salary: i64,
}

fn main() {
    let cities = ["paris", "tokyo", "lima", "oslo"];
    let depts: Vec<Dept> = (0..40).map(|d| Dept { id: d, city: cities[d * 3 % 4] }).collect();
    let staff: Vec<Employee> = (0..60_000)
        .map(|i| Employee { name: format!("e{}", i), dept: i * 7 % 40, salary: 30_000 + (i * 37 % 9000) as i64 })
        .collect();
    let city_of: HashMap<usize, &str> = depts.iter().map(|d| (d.id, d.city)).collect();
    let mut groups: HashMap<&str, Vec<&Employee>> = HashMap::new();
    for e in &staff {
        groups.entry(city_of[&e.dept]).or_default().push(e);
    }
    let avg = |people: &Vec<&Employee>| people.iter().map(|e| e.salary).sum::<i64>() as f64 / people.len() as f64;
    let mut order: Vec<&str> = groups.keys().copied().collect();
    order.sort_by(|a, b| avg(&groups[b]).partial_cmp(&avg(&groups[a])).unwrap());
    for city in order {
        println!("{} {} {:.2}", city, groups[city].len(), avg(&groups[city]));
    }
}
