// The n-body simulation of the Jovian planets: energy before and after 50,000 steps.
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

struct Body {
    double x, y, z, vx, vy, vz, mass;
};

const double solar = 4 * std::numbers::pi * std::numbers::pi, year = 365.24;

Body planet(double x, double y, double z, double vx, double vy, double vz, double m) {
    return {x, y, z, vx * year, vy * year, vz * year, m * solar};
}

double energy(const std::vector<Body>& bodies) {
    double e = 0;
    for (size_t i = 0; i < bodies.size(); i++) {
        const Body& b = bodies[i];
        e += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz);
        for (size_t j = i + 1; j < bodies.size(); j++) {
            const Body& c = bodies[j];
            double dx = b.x - c.x, dy = b.y - c.y, dz = b.z - c.z;
            e -= b.mass * c.mass / std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

void advance(std::vector<Body>& bodies, double dt) {
    for (size_t i = 0; i < bodies.size(); i++) {
        Body& b = bodies[i];
        for (size_t j = i + 1; j < bodies.size(); j++) {
            Body& c = bodies[j];
            double dx = b.x - c.x, dy = b.y - c.y, dz = b.z - c.z;
            double d2 = dx * dx + dy * dy + dz * dz;
            double mag = dt / (d2 * std::sqrt(d2));
            b.vx -= dx * c.mass * mag;
            b.vy -= dy * c.mass * mag;
            b.vz -= dz * c.mass * mag;
            c.vx += dx * b.mass * mag;
            c.vy += dy * b.mass * mag;
            c.vz += dz * b.mass * mag;
        }
    }
    for (Body& b : bodies) {
        b.x += dt * b.vx;
        b.y += dt * b.vy;
        b.z += dt * b.vz;
    }
}

int main() {
    std::vector<Body> bodies = {
        {0, 0, 0, 0, 0, 0, solar},
        planet(4.84143144246472090, -1.16032004402742839, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04),
        planet(8.34336671824457987, 4.12479856412430479, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04),
        planet(12.8943695621391310, -15.1111514016986312, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05),
        planet(15.3796971148509165, -25.9193146099879641, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05),
    };
    double px = 0, py = 0, pz = 0;
    for (auto& b : bodies) px += b.vx * b.mass, py += b.vy * b.mass, pz += b.vz * b.mass;
    bodies[0].vx = -px / solar;
    bodies[0].vy = -py / solar;
    bodies[0].vz = -pz / solar;
    std::printf("%.9f\n", energy(bodies));
    for (int i = 0; i < 50000; i++) advance(bodies, 0.01);
    std::printf("%.9f\n", energy(bodies));
}
