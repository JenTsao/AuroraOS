// =============================================================================
// drivers/gpu/gaussian_blur.hpp
//
// AuroraOS RGB565 高斯模糊算子（可分离卷积 / 定点 / 零动态分配）
//
// 设计原则：
//  - 归属 drivers/gpu 图形层，不进内核；依赖方向仅为 Surface → 像素缓冲
//  - 全整数 Q16 权重 + Q8 中间量：M0+/M3 无 FPU 目标同样可用（AGENTS §P5）
//  - 调用方提供借用 scratch，模块绝不分配内存（AGENTS §12 / §3）
//  - 可分离卷积把 O(taps^2) 降为 O(2*taps)；垂直方向用 (2r+1) 行环形窗口，
//    scratch 只随「核宽 × ROI 宽」增长，而不是整屏
//  - 边界取 ROI 内钳制（clamp-to-edge），保证矩形外像素一个都不被读写，
//    也因此支持 dst==src 的原地模糊（毛玻璃背板直接原地处理）
//  - 错误用 BlurResult 显式返回，不静默忽略；任何失败都不写像素
// =============================================================================
#ifndef AURORA_DRIVERS_GPU_GAUSSIAN_BLUR_HPP
#define AURORA_DRIVERS_GPU_GAUSSIAN_BLUR_HPP

#include <stdint.h>
#include <stddef.h>
#include "surface.hpp"

namespace aurora {
namespace gpu {

// 结果码：命令层可把它回写给调用方，避免「批内单条跳过」变成静默失败。
enum class BlurResult : uint8_t {
    ok = 0,
    invalid_surface,
    invalid_kernel,
    empty_region,
    insufficient_scratch,
};

// 17 taps 是资源与效果的折中：r=8 时 σ≈2.67（3σ 截断），
// scratch 上界 = 17 × width × 3 个 uint16_t，仍由调用方静态/栈上提供。
constexpr uint8_t kMaxBlurRadius = 8;
constexpr uint8_t kMaxBlurTaps = 2 * kMaxBlurRadius + 1;
constexpr uint32_t kBlurWeightOne = 65536u; // Q16 的 1.0，权重和恒为此值

// 一维归一化核。Q16 权重，weights[0..taps) 之和恒为 kBlurWeightOne。
struct BlurKernel {
    uint32_t weights[kMaxBlurTaps]{};
    uint8_t radius{0};
    uint8_t taps{0};

    [[nodiscard]] bool valid() const noexcept {
        if (radius < 1u || radius > kMaxBlurRadius || taps != 2u * radius + 1u) {
            return false;
        }
        uint64_t sum = 0;
        for (uint8_t i = 0; i < taps; ++i) {
            sum += weights[i];
        }
        return sum == kBlurWeightOne;
    }
};

namespace detail {

// e^-x 的 Q16 定点近似，x 为 Q16 非负定点数。
//
// 裸机 freestanding 环境不保证 <cmath> 可用，故用「区间缩放 + 7 阶泰勒 +
// 反复平方」：先把 x 折半到 (0,1]，算 e^-t，再平方 shifts 次还原 e^-x。
// 7 阶泰勒在 [0,1] 的截断误差 < 1/8! ≈ 2.5e-5，Q16 下不足 2 个 LSB。
inline uint32_t exp_neg_q16(uint32_t x_q16) noexcept {
    constexpr uint32_t kOne = 65536u;
    constexpr uint32_t kMaxX = 16u * kOne; // e^-16 < 1e-7，Q16 下归零

    if (x_q16 >= kMaxX) {
        return 0;
    }

    uint32_t t = x_q16;
    int shifts = 0;
    while (t > kOne) {
        t = (t + 1u) >> 1;
        ++shifts;
    }

    int64_t term = static_cast<int64_t>(kOne);
    int64_t sum = term;
    for (uint32_t n = 1u; n <= 7u; ++n) {
        term = -(term * static_cast<int64_t>(t) / (static_cast<int64_t>(kOne) * n));
        sum += term;
    }
    if (sum < 0) {
        sum = 0;
    }

    uint32_t v = static_cast<uint32_t>(sum);
    for (int i = 0; i < shifts; ++i) {
        v = static_cast<uint32_t>((static_cast<uint64_t>(v) * v) >> 16);
    }
    return v;
}

// 把任意非负整数权重归一化成 Q16 且总和精确等于 kBlurWeightOne。
// 逐项向下取整后的残差补给中心抽头，保证「均匀色块模糊后仍是同一颜色」。
inline void normalize_kernel(const uint32_t* raw, uint8_t radius, BlurKernel& out) noexcept {
    const uint32_t taps = 2u * radius + 1u;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < taps; ++i) {
        sum += raw[i];
    }

    uint64_t acc = 0;
    for (uint32_t i = 0; i < taps; ++i) {
        const uint32_t w = (sum == 0) ? 0u : static_cast<uint32_t>(static_cast<uint64_t>(raw[i]) * kBlurWeightOne / sum);
        out.weights[i] = w;
        acc += w;
    }

    if (sum != 0) {
        out.weights[radius] += static_cast<uint32_t>(kBlurWeightOne - acc);
    }
    out.radius = radius;
    out.taps = static_cast<uint8_t>(taps);
}

inline uint16_t pack_rgb565(int64_t r, int64_t g, int64_t b) noexcept {
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 0x1Fu) r = 0x1Fu;
    if (g > 0x3Fu) g = 0x3Fu;
    if (b > 0x1Fu) b = 0x1Fu;
    return static_cast<uint16_t>((r << 11u) | (g << 5u) | b);
}

// 水平卷积一行，结果按 Q8 存成 [R,G,B]×width 的 uint16_t 平面。
// x0/w 定义 ROI 横向区间，越界取样钳制到该区间内。
inline void horizontal_row(const uint16_t* buf, int64_t stride, int64_t x0, int64_t w, int64_t y,
                           const BlurKernel& kernel, uint16_t* plane) noexcept {
    const uint16_t* row = buf + y * stride;
    const int64_t last = x0 + w - 1;
    const int64_t r = kernel.radius;

    for (int64_t i = 0; i < w; ++i) {
        int64_t acc[3] = {0, 0, 0};
        for (uint8_t t = 0; t < kernel.taps; ++t) {
            int64_t sx = x0 + i + static_cast<int64_t>(t) - r;
            if (sx < x0) sx = x0;
            if (sx > last) sx = last;
            const uint16_t px = row[sx];
            const int64_t weight = kernel.weights[t];
            acc[0] += weight * ((px >> 11u) & 0x1Fu);
            acc[1] += weight * ((px >> 5u) & 0x3Fu);
            acc[2] += weight * (px & 0x1Fu);
        }
        // Q16 → Q8（四舍五入），最大 63<<8 = 16128，uint16_t 足够。
        plane[i * 3 + 0] = static_cast<uint16_t>((acc[0] + 128) >> 8);
        plane[i * 3 + 1] = static_cast<uint16_t>((acc[1] + 128) >> 8);
        plane[i * 3 + 2] = static_cast<uint16_t>((acc[2] + 128) >> 8);
    }
}

} // namespace detail

// 由 σ（Q8，即 sigma*256）推导 3σ 截断半径，钳制到 [1, kMaxBlurRadius]。
// σ 过大时会被截断，属于有意的资源上限；需要更强模糊请多趟执行。
[[nodiscard]] inline uint8_t radius_from_sigma(uint16_t sigma_q8) noexcept {
    if (sigma_q8 == 0) {
        return 0;
    }
    uint32_t r = (3u * sigma_q8 + 255u) / 256u;
    if (r < 1u) r = 1u;
    if (r > kMaxBlurRadius) r = kMaxBlurRadius;
    return static_cast<uint8_t>(r);
}

// 真高斯核：w(k) = exp(-k²/(2σ²))，纯整数生成，无 <cmath> 依赖。
[[nodiscard]] inline bool make_gaussian_kernel(uint16_t sigma_q8, uint8_t radius, BlurKernel& out) noexcept {
    if (sigma_q8 == 0 || radius < 1u || radius > kMaxBlurRadius) {
        return false;
    }

    // x_k(Q16) = k²/(2σ²)·65536 = k²·2^31 / sigma_q8²
    const uint64_t sigma_sq = static_cast<uint64_t>(sigma_q8) * sigma_q8;
    uint32_t raw[kMaxBlurTaps] = {};
    for (uint8_t t = 0; t < 2u * radius + 1u; ++t) {
        const int32_t d = static_cast<int32_t>(t) - radius;
        const uint64_t k2 = static_cast<uint64_t>(static_cast<int64_t>(d) * d);
        const uint64_t x = k2 * 2147483648ULL / sigma_sq;
        raw[t] = (x >= 0xFFFFFFFFULL) ? 0u : detail::exp_neg_q16(static_cast<uint32_t>(x));
    }

    BlurKernel tmp{};
    detail::normalize_kernel(raw, radius, tmp);
    if (!tmp.valid()) {
        return false;
    }
    out = tmp;
    return true;
}

// 二项式核：C(2r,k)/2^(2r)。由中心极限定理是 σ≈sqrt(r/2) 的高斯近似，
// 完全不需要 exp，适合最弱的 M0+ 目标；radius 显式给出而非由 σ 推导。
[[nodiscard]] inline bool make_binomial_kernel(uint8_t radius, BlurKernel& out) noexcept {
    if (radius < 1u || radius > kMaxBlurRadius) {
        return false;
    }
    const uint32_t n = 2u * radius;
    uint32_t raw[kMaxBlurTaps] = {};
    uint64_t comb = 1;
    for (uint32_t k = 0; k <= n; ++k) {
        raw[k] = static_cast<uint32_t>(comb);
        comb = comb * (n - k) / (k + 1u);
    }

    BlurKernel tmp{};
    detail::normalize_kernel(raw, radius, tmp);
    if (!tmp.valid()) {
        return false;
    }
    out = tmp;
    return true;
}

// scratch 需求，单位为 uint16_t 字数。返回 0 表示参数非法或结果溢出 uint32。
[[nodiscard]] inline uint32_t scratch_words_for(uint8_t radius, uint32_t width) noexcept {
    if (radius < 1u || radius > kMaxBlurRadius || width == 0) {
        return 0;
    }
    const uint64_t need = static_cast<uint64_t>(2u * radius + 1u) * width * 3u;
    if (need > 0xFFFFFFFFULL) {
        return 0;
    }
    return static_cast<uint32_t>(need);
}

// 对 surface 的矩形区域做原地高斯模糊。
//
// 坐标/尺寸可为负或极大值，按 int64 裁剪到 surface 内（与 SoftGpuDevice 一致）。
// scratch 为借用缓冲，容量按 scratch_words_for(kernel.radius, 裁剪后宽度) 计算；
// 生命周期须覆盖本次调用。失败时返回非 ok 且不修改任何像素。
[[nodiscard]] inline BlurResult blur_rgb565(Surface& surface, int32_t x, int32_t y, uint32_t w, uint32_t h,
                                            const BlurKernel& kernel, uint16_t* scratch,
                                            size_t scratch_words) noexcept {
    if (!surface.valid()) {
        return BlurResult::invalid_surface;
    }
    if (!kernel.valid()) {
        return BlurResult::invalid_kernel;
    }

    const int64_t sw = static_cast<int64_t>(surface.get_width());
    const int64_t sh = static_cast<int64_t>(surface.get_height());
    int64_t x0 = x;
    int64_t y0 = y;
    int64_t rw = static_cast<int64_t>(w);
    int64_t rh = static_cast<int64_t>(h);

    if (x0 < 0) {
        rw += x0;
        x0 = 0;
    }
    if (y0 < 0) {
        rh += y0;
        y0 = 0;
    }
    if (rw <= 0 || rh <= 0 || x0 >= sw || y0 >= sh) {
        return BlurResult::empty_region;
    }
    if (x0 + rw > sw) rw = sw - x0;
    if (y0 + rh > sh) rh = sh - y0;

    const uint32_t need = scratch_words_for(kernel.radius, static_cast<uint32_t>(rw));
    if (need == 0 || scratch == nullptr || scratch_words < need) {
        return BlurResult::insufficient_scratch;
    }

    uint16_t* buf = surface.get_buffer();
    const int64_t taps = kernel.taps;
    const int64_t r = kernel.radius;
    const int64_t plane_words = rw * 3;
    const int64_t y1 = y0 + rh - 1;

    // 环形行窗口：行 sy 的水平结果落在槽 (sy-y0) % taps。
    // 处理输出行 y 时窗口恰为 [y-r, y+r]；新行 y+r 覆盖的槽正是刚离开
    // 窗口的 y-r-1（二者相差 taps），因此不会踩到仍要用的行。
    // 由此也可证原地安全：写回第 y 行之前，所有 sy<=y 的水平结果都已入槽，
    // 后续只会读取 sy>=y+1 的原始行。
    int64_t computed = y0 + r;
    if (computed > y1) computed = y1;
    for (int64_t sy = y0; sy <= computed; ++sy) {
        uint16_t* plane = scratch + ((sy - y0) % taps) * plane_words;
        detail::horizontal_row(buf, sw, x0, rw, sy, kernel, plane);
    }

    for (int64_t row = y0; row <= y1; ++row) {
        int64_t target = row + r;
        if (target > y1) target = y1;
        for (int64_t sy = computed + 1; sy <= target; ++sy) {
            uint16_t* plane = scratch + ((sy - y0) % taps) * plane_words;
            detail::horizontal_row(buf, sw, x0, rw, sy, kernel, plane);
        }
        if (target > computed) computed = target;

        uint16_t* out_row = buf + row * sw + x0;
        for (int64_t i = 0; i < rw; ++i) {
            int64_t acc[3] = {0, 0, 0};
            for (uint8_t t = 0; t < kernel.taps; ++t) {
                int64_t sy = row + static_cast<int64_t>(t) - r;
                if (sy < y0) sy = y0;
                if (sy > y1) sy = y1;
                const uint16_t* p = scratch + ((sy - y0) % taps) * plane_words + i * 3;
                const int64_t weight = kernel.weights[t];
                acc[0] += weight * p[0];
                acc[1] += weight * p[1];
                acc[2] += weight * p[2];
            }
            // Q16(Q16权重) × Q8(中间量) = Q24，四舍五入还原为整数通道。
            out_row[i] = detail::pack_rgb565((acc[0] + (1 << 23)) >> 24, (acc[1] + (1 << 23)) >> 24,
                                             (acc[2] + (1 << 23)) >> 24);
        }
    }
    return BlurResult::ok;
}

} // namespace gpu
} // namespace aurora

#endif // AURORA_DRIVERS_GPU_GAUSSIAN_BLUR_HPP
