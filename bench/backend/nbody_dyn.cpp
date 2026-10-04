// n-body with Ixo's dynamic model: bodies are instances with 7 value fields read
// through a class guard, the list is checked, every operation checks its types.
#include "dyn.hpp"
using namespace dyn;

static Class bodyClass{1};
enum { X, Y, Z, VX, VY, VZ, MASS };

static Value body(double x, double y, double z, double vx, double vy, double vz, double m) {
    Instance* b = newInstance(&bodyClass, 7);
    double v[7] = {x, y, z, vx, vy, vz, m};
    for (int k = 0; k < 7; k++) b->fields[k] = F(v[k]);
    return O(b);
}

__attribute__((noinline)) static void advance(Value bodies, Value steps, Value dt) {
    Value count = I((int64_t)static_cast<List*>(bodies.o)->items.size());
    for (Value s = I(0); lt(s, steps); s = add(s, I(1))) {
        for (Value i = I(0); lt(i, count); i = add(i, I(1))) {
            Value a = index(bodies, i);
            for (Value j = add(i, I(1)); lt(j, count); j = add(j, I(1))) {
                Value b = index(bodies, j);
                Value dx = sub(field(a, &bodyClass, X), field(b, &bodyClass, X));
                Value dy = sub(field(a, &bodyClass, Y), field(b, &bodyClass, Y));
                Value dz = sub(field(a, &bodyClass, Z), field(b, &bodyClass, Z));
                Value d2 = add(add(mul(dx, dx), mul(dy, dy)), mul(dz, dz));
                Value mag = div(dt, mul(d2, sqrtv(d2)));
                Value bm = mul(field(b, &bodyClass, MASS), mag);
                field(a, &bodyClass, VX) = sub(field(a, &bodyClass, VX), mul(dx, bm));
                field(a, &bodyClass, VY) = sub(field(a, &bodyClass, VY), mul(dy, bm));
                field(a, &bodyClass, VZ) = sub(field(a, &bodyClass, VZ), mul(dz, bm));
                Value am = mul(field(a, &bodyClass, MASS), mag);
                field(b, &bodyClass, VX) = add(field(b, &bodyClass, VX), mul(dx, am));
                field(b, &bodyClass, VY) = add(field(b, &bodyClass, VY), mul(dy, am));
                field(b, &bodyClass, VZ) = add(field(b, &bodyClass, VZ), mul(dz, am));
            }
        }
        for (Value& b : static_cast<List*>(bodies.o)->items) {
            field(b, &bodyClass, X) = add(field(b, &bodyClass, X), mul(dt, field(b, &bodyClass, VX)));
            field(b, &bodyClass, Y) = add(field(b, &bodyClass, Y), mul(dt, field(b, &bodyClass, VY)));
            field(b, &bodyClass, Z) = add(field(b, &bodyClass, Z), mul(dt, field(b, &bodyClass, VZ)));
        }
    }
}

static Value energy(Value bodies) {
    Value e = F(0.0);
    Value count = I((int64_t)static_cast<List*>(bodies.o)->items.size());
    for (Value i = I(0); lt(i, count); i = add(i, I(1))) {
        Value a = index(bodies, i);
        Value v2 = add(add(mul(field(a, &bodyClass, VX), field(a, &bodyClass, VX)),
                           mul(field(a, &bodyClass, VY), field(a, &bodyClass, VY))),
                       mul(field(a, &bodyClass, VZ), field(a, &bodyClass, VZ)));
        e = add(e, mul(mul(F(0.5), field(a, &bodyClass, MASS)), v2));
        for (Value j = add(i, I(1)); lt(j, count); j = add(j, I(1))) {
            Value b = index(bodies, j);
            Value dx = sub(field(a, &bodyClass, X), field(b, &bodyClass, X));
            Value dy = sub(field(a, &bodyClass, Y), field(b, &bodyClass, Y));
            Value dz = sub(field(a, &bodyClass, Z), field(b, &bodyClass, Z));
            Value d = sqrtv(add(add(mul(dx, dx), mul(dy, dy)), mul(dz, dz)));
            e = sub(e, div(mul(field(a, &bodyClass, MASS), field(b, &bodyClass, MASS)), d));
        }
    }
    return e;
}

int main() {
    const double PI = 3.141592653589793, SM = 4 * PI * PI, D = 365.24;
    auto* l = new List{2, {}};
    l->items = {
        body(0, 0, 0, 0, 0, 0, SM),
        body(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
             1.66007664274403694e-03 * D, 7.69901118419740425e-03 * D, -6.90460016972063023e-05 * D,
             9.54791938424326609e-04 * SM),
        body(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
             -2.76742510726862411e-03 * D, 4.99852801234917238e-03 * D, 2.30417297573763929e-05 * D,
             2.85885980666130812e-04 * SM),
        body(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
             2.96460137564761618e-03 * D, 2.37847173959480950e-03 * D, -2.96589568540237556e-05 * D,
             4.36624404335156298e-05 * SM),
        body(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
             2.68067772490389322e-03 * D, 1.62824170038242295e-03 * D, -9.51592254519715870e-05 * D,
             5.15138902046611451e-05 * SM),
    };
    Value bodies = O(l);
    volatile int64_t steps = 200000;
    advance(bodies, I(steps), F(0.01));
    std::printf("%.9f\n", energy(bodies).f);
}
