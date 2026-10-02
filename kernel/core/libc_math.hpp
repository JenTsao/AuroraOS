// =============================================================================
// libc_math.hpp — Freestanding float-math 纯函数实现（header-only）
//
// 实现放在头文件中，使 kernel/core/libc.cpp 的 extern "C" 桩与宿主单元测试
// 共享同一份代码：libc.cpp 定义了 malloc/exit/memcpy 等符号，无法整体编入
// 宿主测试二进制，而纯函数没有这一障碍。
//
// 依赖约束：仅使用 IEEE-754 float 运算与 __builtin_nanf（GCC/Clang，与
// arch_api.hpp 中 __builtin_clz 的使用先例一致），不依赖宿主 libm。
// =============================================================================

#pragma once

namespace aurora::libc_math {

inline float floorf(float x) {
    if (x >= 2147483647.0f || x <= -2147483648.0f || x != x)
        return x; // 避免 float→int 转换 UB
    int i = static_cast<int>(x);
    return static_cast<float>(x < 0.0f && x != static_cast<float>(i) ? i - 1 : i);
}

inline float powf(float base, float exp) {
    // 极简 powf：仅支持整数指数（freestanding 环境不引入 expf/logf）。
    // 非整数指数返回 NaN 而非静默截断 —— 旧实现会把 2^0.5 的指数截成 0 后返回 1，
    // Lua 的 ^ 运算符（luaconf.h 强制 float 配置）底层正是本函数。
    if (exp == 0.0f)
        return 1.0f; // pow(x, ±0) = 1（与 libm 的 pow(NaN, 0) = 1 语义一致）
    if (base != base || exp != exp)
        return base + exp; // 任一操作数为 NaN 时和为 NaN，完成传播
    if (exp != floorf(exp))
        return __builtin_nanf(""); // 负底数无实数解，正底数需要 expf/logf，显式报告不支持
    // 饱和到 int 范围，避免超范围 float→int 转换 UB。
    // |exp| ≥ 2^31 的可表示整数 float 必为偶数，取偶数饱和值可保持 (-1)^exp 的符号语义，
    // 且 ±inf 指数经此路径得到与 libm 一致的 inf/0/1 结果。
    int e = exp > 2.0e9f ? 2000000000 : exp < -2.0e9f ? -2000000000 : static_cast<int>(exp);
    float b = base;
    if (e < 0) {
        b = 1.0f / base; // 0^-n → ±inf（IEEE 语义），负底数符号由 IEEE 除法保留
        e = -e;
    }
    // 二分快速幂：O(log|e|)。旧实现为 O(|e|) 无界循环，2^1e9 会长时间占用 CPU
    // 且该路径不受 Lua 指令预算约束（单条 VM 指令内完成）。
    float res = 1.0f;
    while (e != 0) {
        if (e & 1)
            res *= b;
        e >>= 1;
        if (e != 0)
            b *= b;
    }
    return res;
}

} // namespace aurora::libc_math
