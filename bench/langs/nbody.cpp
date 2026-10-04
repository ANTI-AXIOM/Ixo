// n-body simulation: float fields on objects, nested loops, sqrt
#include <cmath>
#include <cstdio>
#include <vector>

struct Body { double x, y, z, vx, vy, vz, mass; };

const double PI = 3.141592653589793;
const double SOLAR_MASS = 4 * PI * PI;
const double DAYS = 365.24;

static std::vector<Body> make_bodies() {
    return {
        {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR_MASS},
        {4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
         1.66007664274403694e-03 * DAYS, 7.69901118419740425e-03 * DAYS,
         -6.90460016972063023e-05 * DAYS, 9.54791938424326609e-04 * SOLAR_MASS},
        {8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
         -2.76742510726862411e-03 * DAYS, 4.99852801234917238e-03 * DAYS,
         2.30417297573763929e-05 * DAYS, 2.85885980666130812e-04 * SOLAR_MASS},
        {1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
         2.96460137564761618e-03 * DAYS, 2.37847173959480950e-03 * DAYS,
         -2.96589568540237556e-05 * DAYS, 4.36624404335156298e-05 * SOLAR_MASS},
        {1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
         2.68067772490389322e-03 * DAYS, 1.62824170038242295e-03 * DAYS,
         -9.51592254519715870e-05 * DAYS, 5.15138902046611451e-05 * SOLAR_MASS},
    };
}

static void advance(std::vector<Body>& bodies, int steps, double dt) {
    size_t count = bodies.size();
    for (int step = 0; step < steps; step++) {
        for (size_t i = 0; i < count; i++) {
            Body& a = bodies[i];
            for (size_t j = i + 1; j < count; j++) {
                Body& b = bodies[j];
                double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                double d2 = dx * dx + dy * dy + dz * dz;
                double mag = dt / (d2 * std::sqrt(d2));
                double bm = b.mass * mag;
                a.vx -= dx * bm; a.vy -= dy * bm; a.vz -= dz * bm;
                double am = a.mass * mag;
                b.vx += dx * am; b.vy += dy * am; b.vz += dz * am;
            }
        }
        for (Body& b : bodies) { b.x += dt * b.vx; b.y += dt * b.vy; b.z += dt * b.vz; }
    }
}

static double energy(const std::vector<Body>& bodies) {
    double e = 0.0;
    size_t count = bodies.size();
    for (size_t i = 0; i < count; i++) {
        const Body& a = bodies[i];
        e += 0.5 * a.mass * (a.vx * a.vx + a.vy * a.vy + a.vz * a.vz);
        for (size_t j = i + 1; j < count; j++) {
            const Body& b = bodies[j];
            double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            e -= a.mass * b.mass / std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

int main() {
    std::vector<Body> bodies = make_bodies();
    advance(bodies, 200000, 0.01);
    std::printf("%.9f\n", energy(bodies));
}
