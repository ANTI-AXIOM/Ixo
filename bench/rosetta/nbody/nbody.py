# The n-body simulation of the Jovian planets: energy before and after 50,000 steps.
import math
from dataclasses import dataclass


@dataclass
class Body:
    x: float
    y: float
    z: float
    vx: float
    vy: float
    vz: float
    mass: float


SOLAR = 4 * math.pi * math.pi
YEAR = 365.24


def planet(x, y, z, vx, vy, vz, m):
    return Body(x, y, z, vx * YEAR, vy * YEAR, vz * YEAR, m * SOLAR)


bodies = [
    Body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR),
    planet(4.84143144246472090, -1.16032004402742839, -1.03622044471123109e-01, 1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05, 9.54791938424326609e-04),
    planet(8.34336671824457987, 4.12479856412430479, -4.03523417114321381e-01, -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05, 2.85885980666130812e-04),
    planet(12.8943695621391310, -15.1111514016986312, -2.23307578892655734e-01, 2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05, 4.36624404335156298e-05),
    planet(15.3796971148509165, -25.9193146099879641, 1.79258772950371181e-01, 2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05, 5.15138902046611451e-05),
]


def energy(bodies):
    e = 0.0
    for i, b in enumerate(bodies):
        e += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz)
        for c in bodies[i + 1 :]:
            dx, dy, dz = b.x - c.x, b.y - c.y, b.z - c.z
            e -= b.mass * c.mass / math.sqrt(dx * dx + dy * dy + dz * dz)
    return e


def advance(bodies, dt):
    for i, b in enumerate(bodies):
        for c in bodies[i + 1 :]:
            dx, dy, dz = b.x - c.x, b.y - c.y, b.z - c.z
            d2 = dx * dx + dy * dy + dz * dz
            mag = dt / (d2 * math.sqrt(d2))
            b.vx -= dx * c.mass * mag
            b.vy -= dy * c.mass * mag
            b.vz -= dz * c.mass * mag
            c.vx += dx * b.mass * mag
            c.vy += dy * b.mass * mag
            c.vz += dz * b.mass * mag
    for b in bodies:
        b.x += dt * b.vx
        b.y += dt * b.vy
        b.z += dt * b.vz


sun = bodies[0]
sun.vx = -sum(b.vx * b.mass for b in bodies) / SOLAR
sun.vy = -sum(b.vy * b.mass for b in bodies) / SOLAR
sun.vz = -sum(b.vz * b.mass for b in bodies) / SOLAR
print(f"{energy(bodies):.9f}")
for _ in range(50_000):
    advance(bodies, 0.01)
print(f"{energy(bodies):.9f}")
