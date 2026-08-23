// =============================================================================
// drivers/gpu/soft_gpu.cpp — SoftGpuDevice 实现
//
// 裁剪模型：所有矩形运算在 int64 域进行。坐标可取任意 int32 极值，
// 尺寸可取 uint32 极值，(int64)x + (int64)w 的组合不会回绕。
// 裁剪顺序：先左/上（负方向），后右/下（正方向），任一步宽高归零即退出。
// =============================================================================
#include "soft_gpu.hpp"

namespace aurora {
namespace gpu {

bool SoftGpuDevice::submit(GpuCommand* cmds, size_t count) noexcept {
    if (cmds == nullptr || count == 0) {
        return false;
    }
    if (count > kMaxBatchCommands) {
        return false; // 资源上限：拒绝失控批次
    }

    // 软件后端同步执行；硬件实现此处应为 DMA 环形缓冲入队
    for (size_t i = 0; i < count; ++i) {
        const GpuCommand& cmd = cmds[i];
        if (!cmd.dst_surface || !cmd.dst_surface->valid()) {
            continue; // 单条非法仅跳过
        }

        switch (cmd.opcode) {
        case GpuOpcode::FillRect:
            do_fill_rect(cmd);
            break;
        case GpuOpcode::Blit:
            do_blit(cmd);
            break;
        case GpuOpcode::Blend:
            do_blend(cmd);
            break;
        default:
            break; // 未知操作码：跳过，保持前向兼容
        }
    }
    return true;
}

void SoftGpuDevice::do_fill_rect(const GpuCommand& cmd) noexcept {
    Surface& dst = *cmd.dst_surface;
    uint16_t* buf = dst.get_buffer();
    const int64_t dw = static_cast<int64_t>(dst.get_width());
    const int64_t dh = static_cast<int64_t>(dst.get_height());

    int64_t x0 = cmd.dst_x;
    int64_t y0 = cmd.dst_y;
    int64_t w = static_cast<int64_t>(cmd.width);
    int64_t h = static_cast<int64_t>(cmd.height);

    if (x0 < 0) {
        w += x0;
        x0 = 0;
    }
    if (y0 < 0) {
        h += y0;
        y0 = 0;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    if (x0 + w > dw) {
        w = dw - x0;
    }
    if (y0 + h > dh) {
        h = dh - y0;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    const uint16_t color = cmd.args.fill.color;
    for (int64_t row = 0; row < h; ++row) {
        uint16_t* line = buf + (y0 + row) * dw + x0;
        for (int64_t col = 0; col < w; ++col) {
            line[col] = color;
        }
    }
}

void SoftGpuDevice::do_blit(const GpuCommand& cmd) noexcept {
    Surface* src_surface = cmd.args.blit.src_surface;
    if (!src_surface || !src_surface->valid()) {
        return;
    }

    Surface& dst = *cmd.dst_surface;
    Surface& src = *src_surface;

    int64_t dx = cmd.dst_x;
    int64_t dy = cmd.dst_y;
    int64_t sx = cmd.args.blit.src_x;
    int64_t sy = cmd.args.blit.src_y;
    int64_t w = static_cast<int64_t>(cmd.width);
    int64_t h = static_cast<int64_t>(cmd.height);

    // 负方向裁剪：目标与源同步收缩（平移量守恒）
    if (dx < 0) {
        w += dx;
        sx -= dx;
        dx = 0;
    }
    if (dy < 0) {
        h += dy;
        sy -= dy;
        dy = 0;
    }
    if (sx < 0) {
        w += sx;
        dx -= sx;
        sx = 0;
    }
    if (sy < 0) {
        h += sy;
        dy -= sy;
        sy = 0;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    // 正方向裁剪：分别对目标/源边界取交集
    if (dx + w > static_cast<int64_t>(dst.get_width())) {
        w = static_cast<int64_t>(dst.get_width()) - dx;
    }
    if (dy + h > static_cast<int64_t>(dst.get_height())) {
        h = static_cast<int64_t>(dst.get_height()) - dy;
    }
    if (sx + w > static_cast<int64_t>(src.get_width())) {
        w = static_cast<int64_t>(src.get_width()) - sx;
    }
    if (sy + h > static_cast<int64_t>(src.get_height())) {
        h = static_cast<int64_t>(src.get_height()) - sy;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    uint16_t* dbuf = dst.get_buffer();
    uint16_t* sbuf = src.get_buffer();
    const int64_t dw = static_cast<int64_t>(dst.get_width());
    const int64_t sw = static_cast<int64_t>(src.get_width());

    // memmove 语义：同一缓冲且区域可能重叠时，按相对位移选择遍历方向，
    // 保证先读后写（重叠搬运不破坏未读像素）。不同缓冲时顺序无影响。
    const bool same_buf = (dbuf == sbuf);
    const bool reverse_rows = same_buf && (dy > sy);
    const bool reverse_cols = same_buf && (dx > sx);

    for (int64_t r = 0; r < h; ++r) {
        const int64_t row = reverse_rows ? (h - 1 - r) : r;
        uint16_t* dline = dbuf + (dy + row) * dw + dx;
        const uint16_t* sline = sbuf + (sy + row) * sw + sx;

        if (reverse_cols) {
            for (int64_t c = w - 1; c >= 0; --c) {
                dline[c] = sline[c];
            }
        } else {
            for (int64_t c = 0; c < w; ++c) {
                dline[c] = sline[c];
            }
        }
    }
}

void SoftGpuDevice::do_blend(const GpuCommand& cmd) noexcept {
    Surface* src_surface = cmd.args.blend.src_surface;
    if (!src_surface || !src_surface->valid()) {
        return;
    }

    const uint8_t alpha = cmd.args.blend.alpha;
    if (alpha == 0) {
        return; // 全透：无操作
    }

    Surface& dst = *cmd.dst_surface;
    Surface& src = *src_surface;

    int64_t dx = cmd.dst_x;
    int64_t dy = cmd.dst_y;
    int64_t sx = cmd.args.blend.src_x;
    int64_t sy = cmd.args.blend.src_y;
    int64_t w = static_cast<int64_t>(cmd.width);
    int64_t h = static_cast<int64_t>(cmd.height);

    if (dx < 0) {
        w += dx;
        sx -= dx;
        dx = 0;
    }
    if (dy < 0) {
        h += dy;
        sy -= dy;
        dy = 0;
    }
    if (sx < 0) {
        w += sx;
        dx -= sx;
        sx = 0;
    }
    if (sy < 0) {
        h += sy;
        dy -= sy;
        sy = 0;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    if (dx + w > static_cast<int64_t>(dst.get_width())) {
        w = static_cast<int64_t>(dst.get_width()) - dx;
    }
    if (dy + h > static_cast<int64_t>(dst.get_height())) {
        h = static_cast<int64_t>(dst.get_height()) - dy;
    }
    if (sx + w > static_cast<int64_t>(src.get_width())) {
        w = static_cast<int64_t>(src.get_width()) - sx;
    }
    if (sy + h > static_cast<int64_t>(src.get_height())) {
        h = static_cast<int64_t>(src.get_height()) - sy;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    uint16_t* dbuf = dst.get_buffer();
    const uint16_t* sbuf = src.get_buffer();
    const int64_t dw = static_cast<int64_t>(dst.get_width());
    const int64_t sw = static_cast<int64_t>(src.get_width());

    // 混合为读-改-写，同缓冲重叠时逐像素读后立即写会污染后续像素；
    // 与 blit 相同的方向策略同样适用（每像素先读 src 再写 dst）。
    const bool same_buf = (dbuf == sbuf);
    const bool reverse_rows = same_buf && (dy > sy);
    const bool reverse_cols = same_buf && (dx > sx);

    for (int64_t r = 0; r < h; ++r) {
        const int64_t row = reverse_rows ? (h - 1 - r) : r;
        uint16_t* dline = dbuf + (dy + row) * dw + dx;
        const uint16_t* sline = sbuf + (sy + row) * sw + sx;

        if (alpha == 255) {
            // 不透明快速路径：等价 blit
            if (reverse_cols) {
                for (int64_t c = w - 1; c >= 0; --c) {
                    dline[c] = sline[c];
                }
            } else {
                for (int64_t c = 0; c < w; ++c) {
                    dline[c] = sline[c];
                }
            }
        } else if (reverse_cols) {
            for (int64_t c = w - 1; c >= 0; --c) {
                dline[c] = blend_rgb565(dline[c], sline[c], alpha);
            }
        } else {
            for (int64_t c = 0; c < w; ++c) {
                dline[c] = blend_rgb565(dline[c], sline[c], alpha);
            }
        }
    }
}

uint16_t SoftGpuDevice::blend_rgb565(uint16_t dst, uint16_t src, uint8_t alpha) noexcept {
    const uint16_t dr = (dst >> 11) & 0x1Fu;
    const uint16_t dg = (dst >> 5) & 0x3Fu;
    const uint16_t db = dst & 0x1Fu;
    const uint16_t sr = (src >> 11) & 0x1Fu;
    const uint16_t sg = (src >> 5) & 0x3Fu;
    const uint16_t sb = src & 0x1Fu;

    const uint16_t inv = static_cast<uint16_t>(255u - alpha);
    const uint16_t rr = static_cast<uint16_t>((sr * alpha + dr * inv) / 255u);
    const uint16_t rg = static_cast<uint16_t>((sg * alpha + dg * inv) / 255u);
    const uint16_t rb = static_cast<uint16_t>((sb * alpha + db * inv) / 255u);

    return static_cast<uint16_t>((rr << 11) | (rg << 5) | rb);
}

} // namespace gpu
} // namespace aurora
