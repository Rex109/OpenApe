module;

#include <algorithm>
#include <cmath>

export module openape.math;

// Minimal row-vector / left-handed math matching Direct3D 9 conventions.
export namespace oa
{
    struct Vec3
    {
        float x = 0, y = 0, z = 0;
        Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
        float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
        Vec3 Cross(const Vec3& o) const { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
        float Length() const { return std::sqrt(Dot(*this)); }
        Vec3 Normalized() const { float l = Length(); return l > 1e-12f ? *this * (1.0f / l) : Vec3{ 0, 0, 1 }; }
    };

    struct Mat4
    {
        float m[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

        static Mat4 Identity() { return {}; }

        Mat4 operator*(const Mat4& b) const
        {
            Mat4 r;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j] + m[i][3] * b.m[3][j];
            return r;
        }

        static Mat4 Translation(const Vec3& t)
        {
            Mat4 r; r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z; return r;
        }

        static Mat4 Scaling(float s)
        {
            Mat4 r; r.m[0][0] = r.m[1][1] = r.m[2][2] = s; return r;
        }

        static Mat4 RotationZ(float a)
        {
            Mat4 r; float c = std::cos(a), s = std::sin(a);
            r.m[0][0] = c; r.m[0][1] = s; r.m[1][0] = -s; r.m[1][1] = c; return r;
        }

        static Mat4 LookAtLH(const Vec3& eye, const Vec3& at, const Vec3& up)
        {
            Vec3 z = (at - eye).Normalized();
            Vec3 x = up.Cross(z).Normalized();
            Vec3 y = z.Cross(x);
            Mat4 r;
            r.m[0][0] = x.x; r.m[0][1] = y.x; r.m[0][2] = z.x;
            r.m[1][0] = x.y; r.m[1][1] = y.y; r.m[1][2] = z.y;
            r.m[2][0] = x.z; r.m[2][1] = y.z; r.m[2][2] = z.z;
            r.m[3][0] = -x.Dot(eye); r.m[3][1] = -y.Dot(eye); r.m[3][2] = -z.Dot(eye);
            return r;
        }

        // View matrix for a RIGHT-handed, Z-up world (the game's convention) displayed through Direct3D:
        // identical to LookAtLH with the x axis mirrored, so screen-right is forward x up.
        // Triangles then wind the opposite way on screen, hence D3DCULL_CCW for front faces.
        static Mat4 LookAtZUpRH(const Vec3& eye, const Vec3& at, const Vec3& up)
        {
            Vec3 z = (at - eye).Normalized();
            Vec3 x = z.Cross(up).Normalized();
            Vec3 y = x.Cross(z);
            Mat4 r;
            r.m[0][0] = x.x; r.m[0][1] = y.x; r.m[0][2] = z.x;
            r.m[1][0] = x.y; r.m[1][1] = y.y; r.m[1][2] = z.y;
            r.m[2][0] = x.z; r.m[2][1] = y.z; r.m[2][2] = z.z;
            r.m[3][0] = -x.Dot(eye); r.m[3][1] = -y.Dot(eye); r.m[3][2] = -z.Dot(eye);
            return r;
        }

        static Mat4 PerspectiveFovLH(float fovY, float aspect, float zn, float zf)
        {
            float yS = 1.0f / std::tan(fovY * 0.5f), xS = yS / aspect;
            Mat4 r{};
            for (auto& row : r.m) for (auto& v : row) v = 0;
            r.m[0][0] = xS; r.m[1][1] = yS;
            r.m[2][2] = zf / (zf - zn); r.m[2][3] = 1;
            r.m[3][2] = -zn * zf / (zf - zn);
            return r;
        }
    };

    // Unit quaternion (x, y, z, w). Rotate() maps a bone-local vector into the parent's space.
    struct Quat
    {
        float x = 0, y = 0, z = 0, w = 1;

        Quat Conjugate() const { return { -x, -y, -z, w }; }
        Quat operator*(const Quat& b) const    // (this * b).Rotate(v) == this.Rotate(b.Rotate(v))
        {
            return { w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
                     w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z };
        }
        Vec3 Rotate(const Vec3& v) const
        {
            float tx = 2 * (y * v.z - z * v.y), ty = 2 * (z * v.x - x * v.z), tz = 2 * (x * v.y - y * v.x);
            return { v.x + w * tx + (y * tz - z * ty), v.y + w * ty + (z * tx - x * tz), v.z + w * tz + (x * ty - y * tx) };
        }
        Quat Normalized() const
        {
            float l = std::sqrt(x * x + y * y + z * z + w * w);
            return l > 1e-12f ? Quat{ x / l, y / l, z / l, w / l } : Quat{};
        }
        // Rotation whose local axes, expressed in the parent space, are X, Y and Z (the columns of the matrix).
        static Quat FromAxes(const Vec3& X, const Vec3& Y, const Vec3& Z)
        {
            float m00 = X.x, m10 = X.y, m20 = X.z, m01 = Y.x, m11 = Y.y, m21 = Y.z, m02 = Z.x, m12 = Z.y, m22 = Z.z;
            float tr = m00 + m11 + m22;
            Quat q;
            if (tr > 0) { float s = std::sqrt(tr + 1) * 2; q = { (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s }; }
            else if (m00 > m11 && m00 > m22) { float s = std::sqrt(1 + m00 - m11 - m22) * 2; q = { 0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s }; }
            else if (m11 > m22) { float s = std::sqrt(1 + m11 - m00 - m22) * 2; q = { (m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s }; }
            else { float s = std::sqrt(1 + m22 - m00 - m11) * 2; q = { (m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s }; }
            return q.Normalized();
        }
        static Quat Slerp(const Quat& a, Quat b, float t)
        {
            float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
            if (d < 0) { b = { -b.x, -b.y, -b.z, -b.w }; d = -d; }
            if (d > 0.9995f)
                return Quat{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t }.Normalized();
            float th = std::acos(d), sa = std::sin((1 - t) * th) / std::sin(th), sb = std::sin(t * th) / std::sin(th);
            return { a.x * sa + b.x * sb, a.y * sa + b.y * sb, a.z * sa + b.z * sb, a.w * sa + b.w * sb };
        }
    };

    constexpr float kPi = 3.14159265358979f;
}
