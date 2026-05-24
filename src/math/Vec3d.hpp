#pragma once

#include <cmath>

namespace spacetrains::math {

struct Vec3d {
    double x {0.0};
    double y {0.0};
    double z {0.0};

    [[nodiscard]] double length() const {
        return std::sqrt(x * x + y * y + z * z);
    }

    [[nodiscard]] Vec3d normalized() const {
        const double len = length();
        if (len <= 0.0) {
            return {};
        }
        return {x / len, y / len, z / len};
    }
};

inline Vec3d operator+(const Vec3d& lhs, const Vec3d& rhs) {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

inline Vec3d operator-(const Vec3d& lhs, const Vec3d& rhs) {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

inline Vec3d operator*(const Vec3d& lhs, double scalar) {
    return {lhs.x * scalar, lhs.y * scalar, lhs.z * scalar};
}

inline Vec3d operator*(double scalar, const Vec3d& rhs) {
    return rhs * scalar;
}

inline Vec3d operator/(const Vec3d& lhs, double scalar) {
    return {lhs.x / scalar, lhs.y / scalar, lhs.z / scalar};
}

}  // namespace spacetrains::math
