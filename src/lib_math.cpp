// Math: the math namespace, vec / mat / quat, and random numbers.
#include <chrono>
#include <cmath>

#include "builtins.hpp"

namespace ixo {

static constexpr double kPi = 3.14159265358979323846;
static constexpr double kE = 2.71828182845904523536;

#define NATIVE(name) static Value name([[maybe_unused]] VM& vm, [[maybe_unused]] Value* a, [[maybe_unused]] int n)

static Value newVec(VM& vm, std::vector<double> d) {
    auto* v = vm.alloc<ObjVec>();
    v->v = std::move(d);
    return Value::object(v);
}

static Value newMat(VM& vm, int r, int c) {
    auto* m = vm.alloc<ObjMat>();
    m->rows = r;
    m->cols = c;
    m->d.assign((size_t)r * c, 0.0);
    return Value::object(m);
}

static std::vector<double> numbersOf(VM& vm, Value v, const char* fname) {
    std::vector<double> out;
    if (isObjType(v, OT::Vec)) return as<ObjVec>(v)->v;
    SeqView items;
    if (!seqOf(v, items)) vm.panic(std::string(fname) + "() expected a list of numbers, got " + vm.typeName(v));
    for (Value x : items) {
        if (!x.isNum()) vm.panic(std::string(fname) + "() expected numbers, got a " + vm.typeName(x));
        out.push_back(x.num());
    }
    return out;
}

static ObjVec* VEC(VM& vm, Value v, const char* f) {
    if (!isObjType(v, OT::Vec)) vm.panic(std::string(f) + "() expected a vec, got " + vm.typeName(v));
    return as<ObjVec>(v);
}

// ------------------------------------------------------------------ vec

// vec(3, [1, 0, 0])  or  vec(1, 0, 0)  or  vec([1, 2])
NATIVE(v_new) {
    if (n == 2 && a[0].isInt() && !a[1].isNum()) {
        auto d = numbersOf(vm, a[1], "vec");
        if ((int64_t)d.size() != a[0].i)
            vm.panic("vec(" + std::to_string(a[0].i) + ", ...) got " + std::to_string(d.size()) + " values");
        return newVec(vm, d);
    }
    if (n == 1 && !a[0].isNum()) return newVec(vm, numbersOf(vm, a[0], "vec"));
    std::vector<double> d;
    for (int i = 0; i < n; i++) d.push_back(vm.argNum(a, i, "vec"));
    return newVec(vm, d);
}
NATIVE(v_zero) { return newVec(vm, std::vector<double>((size_t)vm.argInt(a, 0, "vec.zero"), 0.0)); }

static double vlen(const std::vector<double>& d) {
    double s = 0;
    for (double x : d) s += x * x;
    return std::sqrt(s);
}
NATIVE(v_len) { return Value::number(vlen(as<ObjVec>(a[0])->v)); }
NATIVE(v_dim) { return Value::integer((int64_t)as<ObjVec>(a[0])->v.size()); }
NATIVE(v_unit) {
    auto d = as<ObjVec>(a[0])->v;
    double l = vlen(d);
    if (l == 0) return newVec(vm, d);
    for (auto& x : d) x /= l;
    return newVec(vm, d);
}
NATIVE(v_dot) {
    auto& x = as<ObjVec>(a[0])->v;
    auto& y = VEC(vm, a[1], "dot")->v;
    if (x.size() != y.size()) vm.panic("dot() of vecs with different sizes");
    double s = 0;
    for (size_t i = 0; i < x.size(); i++) s += x[i] * y[i];
    return Value::number(s);
}
NATIVE(v_cross) {
    auto& x = as<ObjVec>(a[0])->v;
    auto& y = VEC(vm, a[1], "cross")->v;
    if (x.size() != 3 || y.size() != 3) vm.panic("cross() needs two 3D vecs");
    return newVec(vm, {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]});
}
NATIVE(v_mix) {
    auto& x = as<ObjVec>(a[0])->v;
    auto& y = VEC(vm, a[1], "mix")->v;
    double t = vm.argNum(a, 2, "mix");
    if (x.size() != y.size()) vm.panic("mix() of vecs with different sizes");
    std::vector<double> d(x.size());
    for (size_t i = 0; i < x.size(); i++) d[i] = x[i] + (y[i] - x[i]) * t;
    return newVec(vm, d);
}
NATIVE(v_dist) {
    auto& x = as<ObjVec>(a[0])->v;
    auto& y = VEC(vm, a[1], "dist")->v;
    double s = 0;
    for (size_t i = 0; i < x.size() && i < y.size(); i++) s += (x[i] - y[i]) * (x[i] - y[i]);
    return Value::number(std::sqrt(s));
}
static Value comp(VM& vm, Value v, size_t i, const char* name) {
    auto& d = as<ObjVec>(v)->v;
    if (i >= d.size()) vm.panic(std::string("a ") + std::to_string(d.size()) + "D vec has no ." + name);
    return Value::number(d[i]);
}
NATIVE(v_x) { return comp(vm, a[0], 0, "x"); }
NATIVE(v_y) { return comp(vm, a[0], 1, "y"); }
NATIVE(v_z) { return comp(vm, a[0], 2, "z"); }
NATIVE(v_w) { return comp(vm, a[0], 3, "w"); }
NATIVE(v_list) {
    std::vector<Value> out;
    for (double x : as<ObjVec>(a[0])->v) out.push_back(Value::number(x));
    return vm.newList(std::move(out));
}

// ------------------------------------------------------------------ mat

// mat([rows, cols], values)
NATIVE(mt_new) {
    auto dims = numbersOf(vm, a[0], "mat");
    if (dims.size() != 2) vm.panic("mat() needs [rows, cols] as its first argument");
    int r = (int)dims[0], c = (int)dims[1];
    if (r <= 0 || c <= 0) vm.panic("matrix dimensions must be positive");
    Value m = newMat(vm, r, c);
    if (n > 1) {
        std::vector<double> vals;
        Value src = a[1];
        if (isObjType(src, OT::List) && as<ObjList>(src)->len() &&
            (isObjType(as<ObjList>(src)->get(0), OT::List) || isObjType(as<ObjList>(src)->get(0), OT::Vec))) {
            for (Value row : SeqView(*as<ObjList>(src))) {
                auto rv = numbersOf(vm, row, "mat");
                vals.insert(vals.end(), rv.begin(), rv.end());
            }
        } else {
            vals = numbersOf(vm, src, "mat");
        }
        if ((int)vals.size() != r * c)
            vm.panic("mat() expected " + std::to_string(r * c) + " values, got " + std::to_string(vals.size()));
        as<ObjMat>(m)->d = vals;
    }
    return m;
}
NATIVE(mt_identity) {
    int k = (int)vm.argInt(a, 0, "mat.identity");
    Value m = newMat(vm, k, k);
    for (int i = 0; i < k; i++) as<ObjMat>(m)->at(i, i) = 1;
    return m;
}
NATIVE(mt_zero) { return newMat(vm, (int)vm.argInt(a, 0, "mat.zero"), (int)vm.argInt(a, 1, "mat.zero")); }

static Value matMul(VM& vm, ObjMat* x, ObjMat* y) {
    if (x->cols != y->rows)
        vm.panic("can't multiply a " + std::to_string(x->rows) + "x" + std::to_string(x->cols) + " matrix by a " +
                 std::to_string(y->rows) + "x" + std::to_string(y->cols) + " one");
    Value out = newMat(vm, x->rows, y->cols);
    auto* o = as<ObjMat>(out);
    for (int i = 0; i < x->rows; i++)
        for (int j = 0; j < y->cols; j++) {
            double s = 0;
            for (int k = 0; k < x->cols; k++) s += x->at(i, k) * y->at(k, j);
            o->at(i, j) = s;
        }
    return out;
}

static Value matApply(VM& vm, ObjMat* m, const std::vector<double>& v) {
    // A 4x4 matrix applied to a 3D vec treats it as a point (w = 1).
    if (m->cols == 4 && v.size() == 3) {
        double in[4] = {v[0], v[1], v[2], 1.0}, out[4];
        for (int i = 0; i < 4; i++) {
            out[i] = 0;
            for (int k = 0; k < 4; k++) out[i] += m->at(i, k) * in[k];
        }
        double w = out[3] != 0 ? out[3] : 1;
        return newVec(vm, {out[0] / w, out[1] / w, out[2] / w});
    }
    if ((int)v.size() != m->cols) vm.panic("matrix/vec size mismatch");
    std::vector<double> out((size_t)m->rows, 0.0);
    for (int i = 0; i < m->rows; i++)
        for (int k = 0; k < m->cols; k++) out[i] += m->at(i, k) * v[k];
    return newVec(vm, out);
}

NATIVE(mt_mul) {
    if (isObjType(a[1], OT::Mat)) return matMul(vm, as<ObjMat>(a[0]), as<ObjMat>(a[1]));
    if (isObjType(a[1], OT::Vec)) return matApply(vm, as<ObjMat>(a[0]), as<ObjVec>(a[1])->v);
    vm.panic("mat.mul() expects a mat or vec");
}
NATIVE(mt_apply) { return matApply(vm, as<ObjMat>(a[0]), VEC(vm, a[1], "apply")->v); }
NATIVE(mt_trans) {
    auto* m = as<ObjMat>(a[0]);
    Value out = newMat(vm, m->cols, m->rows);
    for (int i = 0; i < m->rows; i++)
        for (int j = 0; j < m->cols; j++) as<ObjMat>(out)->at(j, i) = m->at(i, j);
    return out;
}
static bool invert(const ObjMat* m, std::vector<double>& inv, double* detOut) {
    int k = m->rows;
    std::vector<double> w(m->d);
    inv.assign((size_t)k * k, 0.0);
    for (int i = 0; i < k; i++) inv[(size_t)i * k + i] = 1;
    double det = 1;
    for (int col = 0; col < k; col++) {
        int piv = col;
        for (int r = col + 1; r < k; r++)
            if (std::fabs(w[(size_t)r * k + col]) > std::fabs(w[(size_t)piv * k + col])) piv = r;
        if (std::fabs(w[(size_t)piv * k + col]) < 1e-12) {
            if (detOut) *detOut = 0;
            return false;
        }
        if (piv != col) {
            for (int j = 0; j < k; j++) {
                std::swap(w[(size_t)piv * k + j], w[(size_t)col * k + j]);
                std::swap(inv[(size_t)piv * k + j], inv[(size_t)col * k + j]);
            }
            det = -det;
        }
        double p = w[(size_t)col * k + col];
        det *= p;
        for (int j = 0; j < k; j++) {
            w[(size_t)col * k + j] /= p;
            inv[(size_t)col * k + j] /= p;
        }
        for (int r = 0; r < k; r++) {
            if (r == col) continue;
            double f = w[(size_t)r * k + col];
            for (int j = 0; j < k; j++) {
                w[(size_t)r * k + j] -= f * w[(size_t)col * k + j];
                inv[(size_t)r * k + j] -= f * inv[(size_t)col * k + j];
            }
        }
    }
    if (detOut) *detOut = det;
    return true;
}
NATIVE(mt_inv) {
    auto* m = as<ObjMat>(a[0]);
    if (m->rows != m->cols) return vm.newError("NotSquare", "only square matrices can be inverted");
    std::vector<double> inv;
    if (!invert(m, inv, nullptr)) return vm.newError("Singular", "the matrix can't be inverted (determinant is 0)");
    Value out = newMat(vm, m->rows, m->cols);
    as<ObjMat>(out)->d = inv;
    return out;
}
NATIVE(mt_det) {
    auto* m = as<ObjMat>(a[0]);
    if (m->rows != m->cols) vm.panic("det() needs a square matrix");
    std::vector<double> inv;
    double det = 0;
    invert(m, inv, &det);
    return Value::number(det);
}
NATIVE(mt_rows) { return Value::integer(as<ObjMat>(a[0])->rows); }
NATIVE(mt_cols) { return Value::integer(as<ObjMat>(a[0])->cols); }
NATIVE(mt_row) {
    auto* m = as<ObjMat>(a[0]);
    int64_t r = vm.argInt(a, 1, "row");
    if (r < 0 || r >= m->rows) vm.panic("row index out of range");
    std::vector<double> d;
    for (int c = 0; c < m->cols; c++) d.push_back(m->at((int)r, c));
    return newVec(vm, d);
}
NATIVE(mt_col) {
    auto* m = as<ObjMat>(a[0]);
    int64_t c = vm.argInt(a, 1, "col");
    if (c < 0 || c >= m->cols) vm.panic("column index out of range");
    std::vector<double> d;
    for (int r = 0; r < m->rows; r++) d.push_back(m->at(r, (int)c));
    return newVec(vm, d);
}
NATIVE(mt_list) {
    std::vector<Value> out;
    for (double x : as<ObjMat>(a[0])->d) out.push_back(Value::number(x));
    return vm.newList(std::move(out));
}
// 4x4 transforms (column vectors, like OpenGL)
NATIVE(mt_translate) {
    auto v = numbersOf(vm, a[0], "mat.translate");
    if (v.size() != 3) vm.panic("mat.translate() needs a 3D vec");
    Value m = newMat(vm, 4, 4);
    auto* mm = as<ObjMat>(m);
    for (int i = 0; i < 4; i++) mm->at(i, i) = 1;
    mm->at(0, 3) = v[0];
    mm->at(1, 3) = v[1];
    mm->at(2, 3) = v[2];
    return m;
}
NATIVE(mt_scale) {
    std::vector<double> s = a[0].isNum() ? std::vector<double>{a[0].num(), a[0].num(), a[0].num()}
                                         : numbersOf(vm, a[0], "mat.scale");
    if (s.size() != 3) vm.panic("mat.scale() needs a number or a 3D vec");
    Value m = newMat(vm, 4, 4);
    auto* mm = as<ObjMat>(m);
    mm->at(0, 0) = s[0];
    mm->at(1, 1) = s[1];
    mm->at(2, 2) = s[2];
    mm->at(3, 3) = 1;
    return m;
}
NATIVE(mt_rotate) {
    auto ax = numbersOf(vm, a[0], "mat.rotate");
    double ang = vm.argNum(a, 1, "mat.rotate");
    if (ax.size() != 3) vm.panic("mat.rotate() needs a 3D axis");
    double l = vlen(ax);
    if (l == 0) vm.panic("rotation axis can't be zero");
    double x = ax[0] / l, y = ax[1] / l, z = ax[2] / l, c = std::cos(ang), s = std::sin(ang), t = 1 - c;
    Value m = newMat(vm, 4, 4);
    auto* mm = as<ObjMat>(m);
    double r[16] = {t * x * x + c,     t * x * y - s * z, t * x * z + s * y, 0,
                    t * x * y + s * z, t * y * y + c,     t * y * z - s * x, 0,
                    t * x * z - s * y, t * y * z + s * x, t * z * z + c,     0,
                    0,                 0,                 0,                 1};
    mm->d.assign(r, r + 16);
    return m;
}
NATIVE(mt_perspective) {
    double fov = vm.argNum(a, 0, "mat.perspective"), aspect = vm.argNum(a, 1, "mat.perspective");
    double nr = vm.argNum(a, 2, "mat.perspective"), fr = vm.argNum(a, 3, "mat.perspective");
    double f = 1.0 / std::tan(fov / 2);
    Value m = newMat(vm, 4, 4);
    auto* mm = as<ObjMat>(m);
    mm->at(0, 0) = f / aspect;
    mm->at(1, 1) = f;
    mm->at(2, 2) = (fr + nr) / (nr - fr);
    mm->at(2, 3) = 2 * fr * nr / (nr - fr);
    mm->at(3, 2) = -1;
    return m;
}
NATIVE(mt_look_at) {
    auto e = numbersOf(vm, a[0], "mat.look_at"), t = numbersOf(vm, a[1], "mat.look_at"), u = numbersOf(vm, a[2], "mat.look_at");
    if (e.size() != 3 || t.size() != 3 || u.size() != 3) vm.panic("mat.look_at() needs 3D vecs");
    std::vector<double> f = {t[0] - e[0], t[1] - e[1], t[2] - e[2]};
    double fl = vlen(f);
    for (auto& x : f) x /= fl;
    std::vector<double> s = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    double sl = vlen(s);
    for (auto& x : s) x /= sl;
    std::vector<double> uu = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Value m = newMat(vm, 4, 4);
    auto* mm = as<ObjMat>(m);
    for (int i = 0; i < 3; i++) {
        mm->at(0, i) = s[i];
        mm->at(1, i) = uu[i];
        mm->at(2, i) = -f[i];
    }
    mm->at(0, 3) = -(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]);
    mm->at(1, 3) = -(uu[0] * e[0] + uu[1] * e[1] + uu[2] * e[2]);
    mm->at(2, 3) = f[0] * e[0] + f[1] * e[1] + f[2] * e[2];
    mm->at(3, 3) = 1;
    return m;
}

// ------------------------------------------------------------------ quat

static Value newQuat(VM& vm, double x, double y, double z, double w) {
    auto* q = vm.alloc<ObjQuat>();
    q->x = x;
    q->y = y;
    q->z = z;
    q->w = w;
    return Value::object(q);
}
NATIVE(q_new) {
    return newQuat(vm, vm.argNum(a, 0, "quat"), vm.argNum(a, 1, "quat"), vm.argNum(a, 2, "quat"), vm.argNum(a, 3, "quat"));
}
NATIVE(q_axis) {
    auto ax = numbersOf(vm, a[0], "quat.axis");
    double ang = vm.argNum(a, 1, "quat.axis");
    double l = vlen(ax);
    if (ax.size() != 3 || l == 0) vm.panic("quat.axis() needs a non-zero 3D axis");
    double s = std::sin(ang / 2);
    return newQuat(vm, ax[0] / l * s, ax[1] / l * s, ax[2] / l * s, std::cos(ang / 2));
}
NATIVE(q_identity) { return newQuat(vm, 0, 0, 0, 1); }
static Value qmul(VM& vm, ObjQuat* p, ObjQuat* q) {
    return newQuat(vm, p->w * q->x + p->x * q->w + p->y * q->z - p->z * q->y,
                   p->w * q->y - p->x * q->z + p->y * q->w + p->z * q->x,
                   p->w * q->z + p->x * q->y - p->y * q->x + p->z * q->w,
                   p->w * q->w - p->x * q->x - p->y * q->y - p->z * q->z);
}
static Value qturn(VM& vm, ObjQuat* q, const std::vector<double>& v) {
    if (v.size() != 3) vm.panic("a quat can only rotate a 3D vec");
    // v' = v + 2w(q x v) + 2 q x (q x v)
    double qx = q->x, qy = q->y, qz = q->z, qw = q->w;
    double tx = 2 * (qy * v[2] - qz * v[1]), ty = 2 * (qz * v[0] - qx * v[2]), tz = 2 * (qx * v[1] - qy * v[0]);
    return newVec(vm, {v[0] + qw * tx + (qy * tz - qz * ty), v[1] + qw * ty + (qz * tx - qx * tz),
                       v[2] + qw * tz + (qx * ty - qy * tx)});
}
NATIVE(q_mul) {
    if (!isObjType(a[1], OT::Quat)) vm.panic("quat.mul() expects a quat");
    return qmul(vm, as<ObjQuat>(a[0]), as<ObjQuat>(a[1]));
}
NATIVE(q_turn) { return qturn(vm, as<ObjQuat>(a[0]), VEC(vm, a[1], "turn")->v); }
NATIVE(q_conj) {
    auto* q = as<ObjQuat>(a[0]);
    return newQuat(vm, -q->x, -q->y, -q->z, q->w);
}
NATIVE(q_len) {
    auto* q = as<ObjQuat>(a[0]);
    return Value::number(std::sqrt(q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w));
}
NATIVE(q_unit) {
    auto* q = as<ObjQuat>(a[0]);
    double l = std::sqrt(q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
    if (l == 0) return a[0];
    return newQuat(vm, q->x / l, q->y / l, q->z / l, q->w / l);
}
NATIVE(q_mat) {
    auto* q = as<ObjQuat>(a[0]);
    double x = q->x, y = q->y, z = q->z, w = q->w;
    Value m = newMat(vm, 4, 4);
    double r[16] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),     0,
                    2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),     0,
                    2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y), 0,
                    0,                       0,                       0,                       1};
    as<ObjMat>(m)->d.assign(r, r + 16);
    return m;
}
NATIVE(q_slerp) {
    auto* p = as<ObjQuat>(a[0]);
    if (!isObjType(a[1], OT::Quat)) vm.panic("slerp() expects a quat");
    auto* q = as<ObjQuat>(a[1]);
    double t = vm.argNum(a, 2, "slerp");
    double d = p->x * q->x + p->y * q->y + p->z * q->z + p->w * q->w;
    double sgn = d < 0 ? -1 : 1;
    d = std::fabs(d);
    double k0, k1;
    if (d > 0.9995) {
        k0 = 1 - t;
        k1 = t;
    } else {
        double th = std::acos(d), s = std::sin(th);
        k0 = std::sin((1 - t) * th) / s;
        k1 = std::sin(t * th) / s;
    }
    k1 *= sgn;
    return newQuat(vm, p->x * k0 + q->x * k1, p->y * k0 + q->y * k1, p->z * k0 + q->z * k1, p->w * k0 + q->w * k1);
}
NATIVE(q_x) { return Value::number(as<ObjQuat>(a[0])->x); }
NATIVE(q_y) { return Value::number(as<ObjQuat>(a[0])->y); }
NATIVE(q_z) { return Value::number(as<ObjQuat>(a[0])->z); }
NATIVE(q_w) { return Value::number(as<ObjQuat>(a[0])->w); }

// Operators on vec / mat / quat (called from VM::binaryOp).
Value vecBinary(VM& vm, uint8_t op, Value a, Value b, bool* handled) {
    *handled = false;
    bool av = isObjType(a, OT::Vec), bv = isObjType(b, OT::Vec);
    bool am = isObjType(a, OT::Mat), bm = isObjType(b, OT::Mat);
    bool aq = isObjType(a, OT::Quat), bq = isObjType(b, OT::Quat);
    if (!(av || bv || am || bm || aq || bq)) return Value();
    *handled = true;
    if (op == OP_NEG && av) {
        auto d = as<ObjVec>(a)->v;
        for (auto& x : d) x = -x;
        return newVec(vm, d);
    }
    if (av && bv) {
        auto& x = as<ObjVec>(a)->v;
        auto& y = as<ObjVec>(b)->v;
        if (x.size() != y.size()) vm.panic("vec sizes differ (" + std::to_string(x.size()) + " vs " + std::to_string(y.size()) + ")");
        std::vector<double> d(x.size());
        for (size_t i = 0; i < x.size(); i++) {
            switch (op) {
                case OP_ADD: d[i] = x[i] + y[i]; break;
                case OP_SUB: d[i] = x[i] - y[i]; break;
                case OP_MUL: d[i] = x[i] * y[i]; break;
                case OP_DIV: d[i] = x[i] / y[i]; break;
                default: *handled = false; return Value();
            }
        }
        return newVec(vm, d);
    }
    if ((av && b.isNum()) || (bv && a.isNum())) {
        auto d = av ? as<ObjVec>(a)->v : as<ObjVec>(b)->v;
        double s = av ? b.num() : a.num();
        for (auto& x : d) {
            if (op == OP_MUL) x *= s;
            else if (op == OP_DIV && av) x /= s;
            else { *handled = false; return Value(); }
        }
        return newVec(vm, d);
    }
    if (am && bm) {
        auto* x = as<ObjMat>(a);
        auto* y = as<ObjMat>(b);
        if (op == OP_MUL) return matMul(vm, x, y);
        if (op == OP_ADD || op == OP_SUB) {
            if (x->rows != y->rows || x->cols != y->cols) vm.panic("matrix sizes differ");
            Value out = newMat(vm, x->rows, x->cols);
            for (size_t i = 0; i < x->d.size(); i++)
                as<ObjMat>(out)->d[i] = op == OP_ADD ? x->d[i] + y->d[i] : x->d[i] - y->d[i];
            return out;
        }
    }
    if (am && bv && op == OP_MUL) return matApply(vm, as<ObjMat>(a), as<ObjVec>(b)->v);
    if ((am && b.isNum()) || (bm && a.isNum())) {
        if (op == OP_MUL) {
            auto* m = am ? as<ObjMat>(a) : as<ObjMat>(b);
            double s = am ? b.num() : a.num();
            Value out = newMat(vm, m->rows, m->cols);
            for (size_t i = 0; i < m->d.size(); i++) as<ObjMat>(out)->d[i] = m->d[i] * s;
            return out;
        }
    }
    if (aq && bq && op == OP_MUL) return qmul(vm, as<ObjQuat>(a), as<ObjQuat>(b));
    if (aq && bv && op == OP_MUL) return qturn(vm, as<ObjQuat>(a), as<ObjVec>(b)->v);
    *handled = false;
    return Value();
}

// ------------------------------------------------------------------ random


static ObjRng* RNG(VM& vm, Value* a, int n, bool isMethod) {
    if (isMethod && n > 0 && isObjType(a[0], OT::Rng)) return as<ObjRng>(a[0]);
    (void)vm;
    return vm.defaultRng;
}

NATIVE(rng_new) {
    uint64_t seed = n > 0 ? (uint64_t)vm.argInt(a, 0, "rng")
                          : (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return Value::object(vm.alloc<ObjRng>(seed));
}
static double rfloat(ObjRng* r) { return std::uniform_real_distribution<double>(0.0, 1.0)(r->gen); }
static Value rint(VM& vm, ObjRng* r, Value* a, int off) {
    int64_t lo = vm.argInt(a, off, "int"), hi = vm.argInt(a, off + 1, "int");
    if (hi < lo) vm.panic("rng.int(min, max) needs min <= max");
    return Value::integer(std::uniform_int_distribution<int64_t>(lo, hi)(r->gen));
}
static Value rpick(VM& vm, ObjRng* r, Value v) {
    SeqView items;
    if (!seqOf(v, items)) vm.panic("pick() expects a list");
    if (items.empty()) return Value();
    return items[std::uniform_int_distribution<size_t>(0, items.size() - 1)(r->gen)];
}
static Value rmix(VM& vm, ObjRng* r, Value v) {
    if (!isObjType(v, OT::List)) vm.panic("mix() expects a list");
    std::vector<Value> items = SeqView(*as<ObjList>(v)).vec();
    for (size_t i = items.size(); i > 1; i--) {
        size_t j = std::uniform_int_distribution<size_t>(0, i - 1)(r->gen);
        std::swap(items[i - 1], items[j]);
    }
    return vm.newList(std::move(items));
}
NATIVE(g_float) { return Value::number(rfloat(vm.defaultRng)); }
NATIVE(g_int) { return rint(vm, vm.defaultRng, a, 0); }
NATIVE(g_pick) { return rpick(vm, vm.defaultRng, a[0]); }
NATIVE(g_mix) { return rmix(vm, vm.defaultRng, a[0]); }
NATIVE(g_seed) {
    vm.defaultRng->gen.seed((uint64_t)vm.argInt(a, 0, "seed"));
    return Value();
}
NATIVE(g_normal) {
    double mean = n > 0 ? vm.argNum(a, 0, "normal") : 0, sd = n > 1 ? vm.argNum(a, 1, "normal") : 1;
    return Value::number(std::normal_distribution<double>(mean, sd)(vm.defaultRng->gen));
}
NATIVE(r_float) { return Value::number(rfloat(RNG(vm, a, n, true))); }
NATIVE(r_int) { return rint(vm, RNG(vm, a, n, true), a, 1); }
NATIVE(r_pick) { return rpick(vm, RNG(vm, a, n, true), a[1]); }
NATIVE(r_mix) { return rmix(vm, RNG(vm, a, n, true), a[1]); }
NATIVE(r_normal) {
    double mean = n > 1 ? vm.argNum(a, 1, "normal") : 0, sd = n > 2 ? vm.argNum(a, 2, "normal") : 1;
    return Value::number(std::normal_distribution<double>(mean, sd)(RNG(vm, a, n, true)->gen));
}

// ------------------------------------------------------------------ math namespace

#define MATH1(name, expr)                                    \
    NATIVE(mm_##name) {                                      \
        double x = vm.argNum(a, 0, #name);                   \
        return Value::number(expr);                          \
    }
MATH1(sin, std::sin(x))
MATH1(cos, std::cos(x))
MATH1(tan, std::tan(x))
MATH1(asin, std::asin(x))
MATH1(acos, std::acos(x))
MATH1(atan, std::atan(x))
MATH1(sqrt, std::sqrt(x))
MATH1(cbrt, std::cbrt(x))
MATH1(ln, std::log(x))
MATH1(log2, std::log2(x))
MATH1(log10, std::log10(x))
MATH1(exp, std::exp(x))
MATH1(radians, x * kPi / 180.0)
MATH1(degrees, x * 180.0 / kPi)
NATIVE(mm_atan2) { return Value::number(std::atan2(vm.argNum(a, 0, "atan2"), vm.argNum(a, 1, "atan2"))); }
NATIVE(mm_hypot) { return Value::number(std::hypot(vm.argNum(a, 0, "hypot"), vm.argNum(a, 1, "hypot"))); }
NATIVE(mm_floor) { return Value::integer((int64_t)std::floor(vm.argNum(a, 0, "floor"))); }
NATIVE(mm_ceil) { return Value::integer((int64_t)std::ceil(vm.argNum(a, 0, "ceil"))); }
NATIVE(mm_gcd) {
    int64_t x = vm.argInt(a, 0, "gcd"), y = vm.argInt(a, 1, "gcd");
    x = x < 0 ? -x : x;
    y = y < 0 ? -y : y;
    while (y) {
        int64_t t = x % y;
        x = y;
        y = t;
    }
    return Value::integer(x);
}
NATIVE(mm_clamp) {
    double x = vm.argNum(a, 0, "clamp"), lo = vm.argNum(a, 1, "clamp"), hi = vm.argNum(a, 2, "clamp");
    if (a[0].isInt() && a[1].isInt() && a[2].isInt()) return Value::integer(std::clamp(a[0].i, a[1].i, a[2].i));
    return Value::number(std::clamp(x, lo, hi));
}
NATIVE(mm_lerp) {
    double x = vm.argNum(a, 0, "lerp"), y = vm.argNum(a, 1, "lerp"), t = vm.argNum(a, 2, "lerp");
    return Value::number(x + (y - x) * t);
}

void registerMath(VM& vm) {
    auto* math = vm.defineNamespace("math", "Math constants and functions (also available as number methods: 2.sqrt).");
    vm.nativeModules.push_back("math");
    vm.nsValue(math, "pi", Value::number(kPi), "float", "3.14159...");
    vm.nsValue(math, "tau", Value::number(2 * kPi), "float", "2 * pi");
    vm.nsValue(math, "e", Value::number(kE), "float", "2.71828...");
    vm.nsValue(math, "inf", Value::number(INFINITY), "float", "Infinity");
    vm.nsValue(math, "nan", Value::number(NAN), "float", "Not a number");
    vm.nsFunc(math, "sin", mm_sin, 1, 1, "fn(num) -> float", "Sine (radians).");
    vm.nsFunc(math, "cos", mm_cos, 1, 1, "fn(num) -> float", "Cosine (radians).");
    vm.nsFunc(math, "tan", mm_tan, 1, 1, "fn(num) -> float", "Tangent (radians).");
    vm.nsFunc(math, "asin", mm_asin, 1, 1, "fn(num) -> float", "Arc sine.");
    vm.nsFunc(math, "acos", mm_acos, 1, 1, "fn(num) -> float", "Arc cosine.");
    vm.nsFunc(math, "atan", mm_atan, 1, 1, "fn(num) -> float", "Arc tangent.");
    vm.nsFunc(math, "atan2", mm_atan2, 2, 2, "fn(num, num) -> float", "atan2(y, x).");
    vm.nsFunc(math, "sqrt", mm_sqrt, 1, 1, "fn(num) -> float", "Square root.");
    vm.nsFunc(math, "cbrt", mm_cbrt, 1, 1, "fn(num) -> float", "Cube root.");
    vm.nsFunc(math, "ln", mm_ln, 1, 1, "fn(num) -> float", "Natural logarithm.");
    vm.nsFunc(math, "log2", mm_log2, 1, 1, "fn(num) -> float", "Base-2 logarithm.");
    vm.nsFunc(math, "log10", mm_log10, 1, 1, "fn(num) -> float", "Base-10 logarithm.");
    vm.nsFunc(math, "exp", mm_exp, 1, 1, "fn(num) -> float", "e to the power x.");
    vm.nsFunc(math, "hypot", mm_hypot, 2, 2, "fn(num, num) -> float", "sqrt(x*x + y*y).");
    vm.nsFunc(math, "floor", mm_floor, 1, 1, "fn(num) -> int", "Rounds down.");
    vm.nsFunc(math, "ceil", mm_ceil, 1, 1, "fn(num) -> int", "Rounds up.");
    vm.nsFunc(math, "gcd", mm_gcd, 2, 2, "fn(int, int) -> int", "Greatest common divisor.");
    vm.nsFunc(math, "clamp", mm_clamp, 3, 3, "fn(num, num, num) -> num", "Limits x to [lo, hi].");
    vm.nsFunc(math, "lerp", mm_lerp, 3, 3, "fn(num, num, num) -> float", "Linear interpolation.");
    vm.nsFunc(math, "radians", mm_radians, 1, 1, "fn(num) -> float", "Degrees to radians.");
    vm.nsFunc(math, "degrees", mm_degrees, 1, 1, "fn(num) -> float", "Radians to degrees.");

    auto* vec = vm.defineNamespace("vec", "Vectors: vec(1, 2, 3) or vec(3, [1, 2, 3]).");
    vec->callable = vm.makeNative("vec", v_new, 1, -1);
    vm.nsFunc(vec, "zero", v_zero, 1, 1, "fn(int) -> vec", "A zero vector with n components.");
    vm.defineMethod(Tag::Vec, "len", v_len, 0, 0, "fn() -> float", "Length (magnitude).");
    vm.defineMethod(Tag::Vec, "dim", v_dim, 0, 0, "fn() -> int", "Number of components.");
    vm.defineMethod(Tag::Vec, "unit", v_unit, 0, 0, "fn() -> vec", "Same direction, length 1.");
    vm.defineMethod(Tag::Vec, "dot", v_dot, 1, 1, "fn(vec) -> float", "Dot product.");
    vm.defineMethod(Tag::Vec, "cross", v_cross, 1, 1, "fn(vec) -> vec", "Cross product (3D).");
    vm.defineMethod(Tag::Vec, "mix", v_mix, 2, 2, "fn(vec, num) -> vec", "Interpolates towards another vec.");
    vm.defineMethod(Tag::Vec, "dist", v_dist, 1, 1, "fn(vec) -> float", "Distance to another vec.");
    vm.defineMethod(Tag::Vec, "x", v_x, 0, 0, "fn() -> float", "First component.");
    vm.defineMethod(Tag::Vec, "y", v_y, 0, 0, "fn() -> float", "Second component.");
    vm.defineMethod(Tag::Vec, "z", v_z, 0, 0, "fn() -> float", "Third component.");
    vm.defineMethod(Tag::Vec, "w", v_w, 0, 0, "fn() -> float", "Fourth component.");
    vm.defineMethod(Tag::Vec, "list", v_list, 0, 0, "fn() -> list[float]", "Components as a list.");

    auto* mat = vm.defineNamespace("mat", "Matrices: mat([rows, cols], values), mat.identity(4), transforms.");
    mat->callable = vm.makeNative("mat", mt_new, 1, 2);
    vm.nsFunc(mat, "identity", mt_identity, 1, 1, "fn(int) -> mat", "Identity matrix.");
    vm.nsFunc(mat, "zero", mt_zero, 2, 2, "fn(int, int) -> mat", "All-zero matrix.");
    vm.nsFunc(mat, "translate", mt_translate, 1, 1, "fn(vec) -> mat", "4x4 translation.");
    vm.nsFunc(mat, "scale", mt_scale, 1, 1, "fn(num) -> mat", "4x4 scale (number or vec).");
    vm.nsFunc(mat, "rotate", mt_rotate, 2, 2, "fn(vec, num) -> mat", "4x4 rotation around an axis (radians).");
    vm.nsFunc(mat, "perspective", mt_perspective, 4, 4, "fn(num, num, num, num) -> mat", "Perspective projection (fov, aspect, near, far).");
    vm.nsFunc(mat, "look_at", mt_look_at, 3, 3, "fn(vec, vec, vec) -> mat", "View matrix (eye, target, up).");
    vm.defineMethod(Tag::Mat, "mul", mt_mul, 1, 1, "fn(mat) -> mat", "Matrix product (or applies to a vec).");
    vm.defineMethod(Tag::Mat, "apply", mt_apply, 1, 1, "fn(vec) -> vec", "Transforms a vec.");
    vm.defineMethod(Tag::Mat, "trans", mt_trans, 0, 0, "fn() -> mat", "Transpose.");
    vm.defineMethod(Tag::Mat, "inv", mt_inv, 0, 0, "fn() -> mat ! Error", "Inverse (Error.Singular if impossible).");
    vm.defineMethod(Tag::Mat, "det", mt_det, 0, 0, "fn() -> float", "Determinant.");
    vm.defineMethod(Tag::Mat, "rows", mt_rows, 0, 0, "fn() -> int", "Number of rows.");
    vm.defineMethod(Tag::Mat, "cols", mt_cols, 0, 0, "fn() -> int", "Number of columns.");
    vm.defineMethod(Tag::Mat, "row", mt_row, 1, 1, "fn(int) -> vec", "A row as a vec.");
    vm.defineMethod(Tag::Mat, "col", mt_col, 1, 1, "fn(int) -> vec", "A column as a vec.");
    vm.defineMethod(Tag::Mat, "list", mt_list, 0, 0, "fn() -> list[float]", "Values in row-major order.");

    auto* quat = vm.defineNamespace("quat", "Quaternions: quat(x, y, z, w), quat.axis(axis, angle).");
    quat->callable = vm.makeNative("quat", q_new, 4, 4);
    vm.nsFunc(quat, "axis", q_axis, 2, 2, "fn(vec, num) -> quat", "Rotation of angle (radians) around an axis.");
    vm.nsFunc(quat, "identity", q_identity, 0, 0, "fn() -> quat", "No rotation.");
    vm.defineMethod(Tag::Quat, "mul", q_mul, 1, 1, "fn(quat) -> quat", "Combines rotations.");
    vm.defineMethod(Tag::Quat, "turn", q_turn, 1, 1, "fn(vec) -> vec", "Rotates a 3D vec.");
    vm.defineMethod(Tag::Quat, "conj", q_conj, 0, 0, "fn() -> quat", "Conjugate (inverse rotation for unit quats).");
    vm.defineMethod(Tag::Quat, "len", q_len, 0, 0, "fn() -> float", "Norm.");
    vm.defineMethod(Tag::Quat, "unit", q_unit, 0, 0, "fn() -> quat", "Normalized copy.");
    vm.defineMethod(Tag::Quat, "mat", q_mat, 0, 0, "fn() -> mat", "4x4 rotation matrix.");
    vm.defineMethod(Tag::Quat, "slerp", q_slerp, 2, 2, "fn(quat, num) -> quat", "Spherical interpolation.");
    vm.defineMethod(Tag::Quat, "x", q_x, 0, 0, "fn() -> float", "x");
    vm.defineMethod(Tag::Quat, "y", q_y, 0, 0, "fn() -> float", "y");
    vm.defineMethod(Tag::Quat, "z", q_z, 0, 0, "fn() -> float", "z");
    vm.defineMethod(Tag::Quat, "w", q_w, 0, 0, "fn() -> float", "w");

    vm.defaultRng = vm.alloc<ObjRng>((uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count() +
                                     vm.heapId * 7919);
    auto* rng = vm.defineNamespace("rng", "Random numbers: rng.float, rng.int(1, 6), rng.pick(list); rng(seed) makes a generator.");
    rng->callable = vm.makeNative("rng", rng_new, 0, 1);
    vm.nsFunc(rng, "float", g_float, 0, 0, "fn() -> float", "Random float in [0, 1).");
    vm.nsFunc(rng, "int", g_int, 2, 2, "fn(int, int) -> int", "Random int in [min, max].");
    vm.nsFunc(rng, "pick", g_pick, 1, 1, "fn(list) -> any", "Random element.");
    vm.nsFunc(rng, "mix", g_mix, 1, 1, "fn(list) -> list", "Shuffled copy.");
    vm.nsFunc(rng, "seed", g_seed, 1, 1, "fn(int)", "Reseeds the default generator.");
    vm.nsFunc(rng, "normal", g_normal, 0, 2, "fn(num?, num?) -> float", "Normally distributed number.");
    vm.defineMethod(Tag::Rng, "float", r_float, 0, 0, "fn() -> float", "Random float in [0, 1).");
    vm.defineMethod(Tag::Rng, "int", r_int, 2, 2, "fn(int, int) -> int", "Random int in [min, max].");
    vm.defineMethod(Tag::Rng, "pick", r_pick, 1, 1, "fn(list) -> any", "Random element.");
    vm.defineMethod(Tag::Rng, "mix", r_mix, 1, 1, "fn(list) -> list", "Shuffled copy.");
    vm.defineMethod(Tag::Rng, "normal", r_normal, 0, 2, "fn(num?, num?) -> float", "Normally distributed number.");
}

}  // namespace ixo
