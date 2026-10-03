// =============================================================================
// test_libc_math.cpp — kernel freestanding math stubs (libc_math.hpp)
//
// 重点回归：powf 对非整数指数必须返回 NaN（旧实现静默截断指数，2^0.5 → 1，
// 经由 Lua ^ 运算符传导），且大指数必须 O(log|e|) 有界完成（旧实现为
// O(|e|) 无界循环，且该路径不受 Lua 指令预算约束）。
// =============================================================================

#include <gtest/gtest.h>
#include <limits>

#include "libc_math.hpp"

using aurora::libc_math::floorf;
using aurora::libc_math::powf;

namespace {

bool is_nanf(float v) {
    return v != v;
}

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kQNaN = std::numeric_limits<float>::quiet_NaN();

} // namespace

// --- 整数指数：结果保持正确 ---
TEST(LibcMath, PowfIntegerExponents) {
    EXPECT_FLOAT_EQ(powf(2.0f, 10.0f), 1024.0f);
    EXPECT_FLOAT_EQ(powf(2.0f, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(powf(7.0f, 1.0f), 7.0f);
    EXPECT_FLOAT_EQ(powf(-2.0f, 3.0f), -8.0f);  // 负底数奇次幂
    EXPECT_FLOAT_EQ(powf(-2.0f, 2.0f), 4.0f);   // 负底数偶次幂
    EXPECT_FLOAT_EQ(powf(2.0f, -2.0f), 0.25f);
    EXPECT_FLOAT_EQ(powf(-2.0f, -3.0f), -0.125f);
}

// --- 回归：非整数指数返回 NaN，而非截断后给出错误结果 ---
TEST(LibcMath, PowfFractionalExponentReturnsNaN) {
    EXPECT_TRUE(is_nanf(powf(2.0f, 0.5f)));   // 旧实现：截断为 0 → 返回 1
    EXPECT_TRUE(is_nanf(powf(9.0f, 0.5f)));   // 旧实现：1（正确值应为 3）
    EXPECT_TRUE(is_nanf(powf(2.0f, -0.5f)));  // 负分数指数同样不支持
    EXPECT_TRUE(is_nanf(powf(-2.0f, 0.5f)));  // 负底数分数次幂无实数解
    EXPECT_TRUE(is_nanf(powf(-8.0f, 1.0f / 3.0f))); // 1/3 不可精确表示
}

// --- NaN 传播 ---
TEST(LibcMath, PowfNaNPropagation) {
    EXPECT_TRUE(is_nanf(powf(kQNaN, 5.0f)));
    EXPECT_TRUE(is_nanf(powf(2.0f, kQNaN)));
    EXPECT_FLOAT_EQ(powf(kQNaN, 0.0f), 1.0f); // libm: pow(NaN, 0) = 1
}

// --- 大/无穷指数：饱和路径与 libm 语义一致，且必须立即返回 ---
TEST(LibcMath, PowfHugeAndInfiniteExponents) {
    EXPECT_EQ(powf(2.0f, 2000000000.0f), kInf);  // 旧实现需循环 20 亿次
    EXPECT_EQ(powf(0.5f, 2000000000.0f), 0.0f);
    EXPECT_EQ(powf(2.0f, -2000000000.0f), 0.0f);
    EXPECT_FLOAT_EQ(powf(-1.0f, 2000000000.0f), 1.0f); // 大指数整数 float 必为偶数
    EXPECT_EQ(powf(2.0f, kInf), kInf);
    EXPECT_EQ(powf(0.5f, kInf), 0.0f);
    EXPECT_FLOAT_EQ(powf(-1.0f, kInf), 1.0f);
    EXPECT_EQ(powf(2.0f, -kInf), 0.0f);
}

// --- 零底数 ---
TEST(LibcMath, PowfZeroBase) {
    EXPECT_FLOAT_EQ(powf(0.0f, 5.0f), 0.0f);
    EXPECT_EQ(powf(0.0f, -2.0f), kInf);
    EXPECT_FLOAT_EQ(powf(0.0f, 0.0f), 1.0f);
}

// --- floorf（随 powf 一并下沉到 libc_math.hpp）---
TEST(LibcMath, FloorfBasics) {
    EXPECT_FLOAT_EQ(floorf(2.5f), 2.0f);
    EXPECT_FLOAT_EQ(floorf(-2.5f), -3.0f);
    EXPECT_FLOAT_EQ(floorf(3.0f), 3.0f);
    EXPECT_FLOAT_EQ(floorf(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(floorf(1e20f), 1e20f); // 超出 int 范围原样返回，避免转换 UB
}
