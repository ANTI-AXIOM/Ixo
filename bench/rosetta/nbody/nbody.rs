// The n-body simulation of the Jovian planets: energy before and after 50,000 steps.
use std::f64::consts::PI;

struct Body {
    x: f64,
    y: f64,
    z: f64,
    vx: f64,
    vy: f64,
    vz: f64,
    mass: f64,
}

const SOLAR: f64 = 4.0 * PI * PI;
const YEAR: f64 = 365.24;

fn planet(x: f64, y: f64, z: f64, vx: f64, vy: f64, vz: f64, m: f64) -> Body {
    Body { x, y, z, vx: vx * YEAR, vy: vy * YEAR, vz: vz * YEAR, mass: m * SOLAR }
}

fn energy(bodies: &[Body]) -> f64 {
    let mut e = 0.0;
    for (i, b) in bodies.iter().enumerate() {
        e += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz);
        for c in &bodies[i + 1..] {
            let (dx, dy, dz) = (b.x - c.x, b.y - c.y, b.z - c.z);
            e -= b.mass * c.mass / (dx * dx + dy * dy + dz * dz).sqrt();
        }
    }
    e
}

fn advance(bodies: &mut [Body], dt: f64) {
    for i in 0..bodies.len() {
        let (left, right) = bodies.split_at_mut(i + 1);
        let b = &mut left[i];
        for c in right.iter_mut() {
            let (dx, dy, dz) = (b.x - c.x, b.y - c.y, b.z - c.z);
            let d2 = dx * dx + dy * dy + dz * dz;
            let mag = dt / (d2 * d2.sqrt());
            b.vx -= dx * c.mass * mag;
            b.vy -= dy * c.mass * mag;
            b.vz -= dz * c.mass * mag;
            c.vx += dx * b.mass * mag;
            c.vy += dy * b.mass * mag;
            c.vz += dz * b.mass * mag;
        }
    }
    for b in bodies.iter_mut() {
        b.x += dt * b.vx;
        b.y += dt * b.vy;
        b.z += dt * b.vz;
    }
}

fn main() {
    let mut bodies = vec![
        Body { x: 0.0, y: 0.0, z: 0.0, vx: 0.0, vy: 0.0, vz: 0.0, mass: SOLAR },
        planet(4.84143144246472090, -1.16032004402742839, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04),
        planet(8.34336671824457987, 4.12479856412430479, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04),
        planet(12.8943695621391310, -15.1111514016986312, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05),
        planet(15.3796971148509165, -25.9193146099879641, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05),
    ];
    let (px, py, pz) = bodies.iter().fold((0.0, 0.0, 0.0), |p, b| (p.0 + b.vx * b.mass, p.1 + b.vy * b.mass, p.2 + b.vz * b.mass));
    bodies[0].vx = -px / SOLAR;
    bodies[0].vy = -py / SOLAR;
    bodies[0].vz = -pz / SOLAR;
    println!("{:.9}", energy(&bodies));
    for _ in 0..50_000 {
        advance(&mut bodies, 0.01);
    }
    println!("{:.9}", energy(&bodies));
}
