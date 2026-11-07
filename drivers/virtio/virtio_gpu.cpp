// =============================================================================
// drivers/virtio/virtio_gpu.cpp — virtio-gpu 2D 显示后端实现
// =============================================================================
#include "virtio_gpu.hpp"
#include "virtio_mmio.hpp"
#include "board.h"

namespace auroraos {
namespace virtio {

VirtioGpuDisplay& VirtioGpuDisplay::instance() {
    // 槽位按设备类型扫描（见 virtio_scan_mmio 注释），board.h 的 SLOT 只作回退。
    static VirtioMmioTransport mmio(auroraos::virtio::virtio_scan_mmio(
        BOARD_VIRTIO_MMIO_BASE, BOARD_VIRTIO_MMIO_COUNT, BOARD_VIRTIO_MMIO_STRIDE, auroraos::virtio::DeviceId::Gpu,
        BOARD_VIRTIO_MMIO_BASE + BOARD_VIRTIO_GPU_SLOT * BOARD_VIRTIO_MMIO_STRIDE));
    static VirtioGpuDisplay dev(DISPLAY_WIDTH, DISPLAY_HEIGHT, BOARD_VIRTIO_GPU_SCALE, mmio, true);
    return dev;
}

uint32_t gpu_pixel_to_b8g8r8x8(uint16_t pixel, bool mono1bit) noexcept {
    if (mono1bit) {
        // 单色阈值：≥0x8410（约中灰）→ 白，否则黑
        return pixel >= 0x8410u ? 0x00FFFFFFu : 0x00000000u;
    }
    const uint32_t r5 = (pixel >> 11) & 0x1F;
    const uint32_t g6 = (pixel >> 5) & 0x3F;
    const uint32_t b5 = pixel & 0x1F;
    // 5:6:5 → 8:8:8 位复制扩展；B8G8R8X8 小端字 = 0x00RRGGBB
    const uint32_t r8 = (r5 << 3) | (r5 >> 2);
    const uint32_t g8 = (g6 << 2) | (g6 >> 4);
    const uint32_t b8 = (b5 << 3) | (b5 >> 2);
    return (r8 << 16) | (g8 << 8) | b8;
}

uint32_t gpu_blit_scaled(uint8_t* fb, uint32_t fb_w,
                         const uint16_t* src, uint32_t count,
                         uint16_t win_x0, uint16_t win_y0, uint16_t win_x1,
                         uint32_t scale, bool mono1bit,
                         uint16_t& cur_x, uint16_t& cur_y) noexcept {
    if (!fb || !src || scale == 0 || fb_w == 0) {
        return 0;
    }

    uint32_t written = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (cur_x > win_x1) {
            cur_x = win_x0;
            cur_y = static_cast<uint16_t>(cur_y + 1);
        }
        const uint32_t color = gpu_pixel_to_b8g8r8x8(src[i], mono1bit);

        // 放大写入 scale×scale 块（物理坐标 = 逻辑坐标 × scale）
        for (uint32_t sy = 0; sy < scale; ++sy) {
            auto* row = reinterpret_cast<uint32_t*>(
                fb + (((static_cast<uint64_t>(cur_y) * scale) + sy) * fb_w +
                      static_cast<uint64_t>(cur_x) * scale) *
                         4);
            for (uint32_t sx = 0; sx < scale; ++sx) {
                row[sx] = color;
            }
        }

        cur_x = static_cast<uint16_t>(cur_x + 1);
        ++written;
    }
    return written;
}

bool VirtioGpuDisplay::init() noexcept {
    const uint32_t phys_w = static_cast<uint32_t>(logical_w_) * scale_;
    const uint32_t phys_h = static_cast<uint32_t>(logical_h_) * scale_;
    if (scale_ == 0 || phys_w > FB_MAX_W || phys_h > FB_MAX_H) {
        return false;
    }

    if (!virtio_probe(transport_, DeviceId::Gpu)) {
        return false;
    }
    probed_ = true;

    virtio_reset(transport_);
    virtio_status_add(transport_, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    // 初始化失败统一回滚：置 STATUS_FAILED；若已建资源则先 RESOURCE_UNREF，
    // 保证重试 init 不会残留同 ID 资源冲突。
    bool resource_created = false;
    auto fail = [&]() noexcept -> bool {
        if (resource_created) {
            GpuCmdResourceUnref unref{};
            unref.hdr.type = GPU_CMD_RESOURCE_UNREF;
            unref.resource_id = RESOURCE_ID;
            uint32_t resp = 0;
            submit_cmd(&unref, sizeof(unref), sizeof(GpuCmdHeader), &resp);
        }
        virtio_status_add(transport_, STATUS_FAILED);
        return false;
    };

    if (!virtio_negotiate_features(transport_, 0, 0, 0, 0, nullptr, nullptr) ||
        !virtio_features_ok(transport_)) {
        return fail();
    }

    const uint16_t qsz =
        virtio_queue_configure(transport_, 0, QUEUE_SIZE, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(desc_)),
                               static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&avail_)),
                               static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&used_)));
    if (qsz == 0) {
        return fail();
    }
    VirtQueue::Rings rings{desc_, &avail_, &used_};
    if (!queue_.init(rings, qsz, 0, transport_)) {
        return fail();
    }

    uint32_t resp = 0;

    // 1. GET_DISPLAY_INFO：确认 scanout 0 存在
    GpuCmdHeader hdr{};
    hdr.type = GPU_CMD_GET_DISPLAY_INFO;
    if (!submit_cmd(&hdr, sizeof(hdr), GPU_DISPLAY_INFO_RESP_SIZE, &resp) ||
        resp != GPU_RESP_OK_DISPLAY_INFO) {
        return fail();
    }

    // 2. RESOURCE_CREATE_2D：B8G8R8X8 全屏资源
    GpuCmdCreate2D create{};
    create.hdr.type = GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = RESOURCE_ID;
    create.format = GPU_FORMAT_B8G8R8X8_UNORM;
    create.width = phys_w;
    create.height = phys_h;
    if (!submit_cmd(&create, sizeof(create), sizeof(GpuCmdHeader), &resp) ||
        resp != GPU_RESP_OK_NODATA) {
        return fail();
    }
    resource_created = true;

    // 3. RESOURCE_ATTACH_BACKING：绑定静态显存
    GpuCmdAttachBacking attach{};
    attach.hdr.type = GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.resource_id = RESOURCE_ID;
    attach.nr_entries = 1;
    attach.entries[0].mem_offset = 0;
    attach.entries[0].length = phys_w * phys_h * 4;
    if (!submit_cmd(&attach, sizeof(attach), sizeof(GpuCmdHeader), &resp) ||
        resp != GPU_RESP_OK_NODATA) {
        return fail();
    }

    // 4. SET_SCANOUT：scanout 0 → 资源（仅初始化时设置一次）
    GpuCmdSetScanout scanout{};
    scanout.hdr.type = GPU_CMD_SET_SCANOUT;
    scanout.rect = GpuRect{0, 0, phys_w, phys_h};
    scanout.scanout_id = 0;
    scanout.resource_id = RESOURCE_ID;
    if (!submit_cmd(&scanout, sizeof(scanout), sizeof(GpuCmdHeader), &resp) ||
        resp != GPU_RESP_OK_NODATA) {
        return fail();
    }

    virtio_driver_ok(transport_);
    ready_ = true;
    return true;
}

bool VirtioGpuDisplay::submit_cmd(const void* cmd, uint32_t cmd_len, uint32_t resp_len,
                                  uint32_t* resp_type) noexcept {
    if (cmd_len > sizeof(cmd_buf_) || resp_len > sizeof(resp_buf_)) {
        return false;
    }
    if (resp_len < sizeof(GpuCmdHeader)) {
        return false;
    }
    __builtin_memcpy(cmd_buf_, cmd, cmd_len);

    BufSeg segs[2] = {
        {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cmd_buf_)), cmd_len, false},
        {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(resp_buf_)), resp_len, true},
    };

    dma_sync_for_device(cmd_buf_, cmd_len);
    const int head = queue_.add_buffer(segs, 2);
    if (head < 0) {
        return false;
    }
    queue_.notify();

    uint32_t len = 0;
    for (uint32_t spin = 0; spin < MAX_POLL_SPINS; ++spin) {
        const int done_head = queue_.pop_used(&len);
        if (done_head >= 0) {
            if (done_head != head) {
                // 孤儿完成：保守失败并停用设备，防止在途缓冲被复用损坏
                virtio_status_add(transport_, STATUS_FAILED);
                ready_ = false;
                return false;
            }
            dma_sync_for_cpu(resp_buf_, resp_len);
            uint32_t type = 0;
            __builtin_memcpy(&type, resp_buf_, sizeof(type));
            if (resp_type) {
                *resp_type = type;
            }
            // 响应码 0x11xx = 成功，0x12xx = 设备错误
            return (type & 0xFF00u) == 0x1100u;
        }
    }
    // 超时：不挂死；置失败态防止 cmd_buf_/resp_buf_ 被迟到 DMA 回写打坏
    virtio_status_add(transport_, STATUS_FAILED);
    ready_ = false;
    return false;
}

void VirtioGpuDisplay::set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    // 窗口必须是逻辑屏内的合法矩形（闭区间）：倒置或越界一律拒绝（空窗口），
    // 防止 win_pixels_ 下溢后 write_patch/gpu_blit_scaled 越出 fb_。
    if (x1 < x0 || y1 < y0 || x1 >= logical_w_ || y1 >= logical_h_) {
        win_pixels_ = 0;
        got_pixels_ = 0;
        return;
    }
    win_x0_ = x0;
    win_y0_ = y0;
    win_x1_ = x1;
    win_y1_ = y1;
    cur_x_ = x0;
    cur_y_ = y0;
    win_pixels_ = static_cast<uint32_t>(x1 - x0 + 1) * (y1 - y0 + 1);
    got_pixels_ = 0;
}

void VirtioGpuDisplay::write_patch(const uint16_t* buffer, uint32_t pixel_count) {
    if (!ready_ || !buffer || win_pixels_ == 0) {
        return;
    }

    const uint32_t remaining = win_pixels_ - got_pixels_;
    uint32_t take = pixel_count < remaining ? pixel_count : remaining;
    take = gpu_blit_scaled(fb_, logical_w_ * scale_, buffer, take,
                           win_x0_, win_y0_, win_x1_, scale_, mono1bit_, cur_x_, cur_y_);
    got_pixels_ += take;

    if (got_pixels_ >= win_pixels_) {
        commit_window();
        win_pixels_ = 0; // 忽略窗口外的多余像素
    }
}

void VirtioGpuDisplay::commit_window() noexcept {
    const uint32_t phys_w = static_cast<uint32_t>(logical_w_) * scale_;
    const uint32_t rect_x = static_cast<uint32_t>(win_x0_) * scale_;
    const uint32_t rect_y = static_cast<uint32_t>(win_y0_) * scale_;
    const uint32_t rect_w = static_cast<uint32_t>(win_x1_ - win_x0_ + 1) * scale_;
    const uint32_t rect_h = static_cast<uint32_t>(win_y1_ - win_y0_ + 1) * scale_;

    last_rect_ = GpuRect{rect_x, rect_y, rect_w, rect_h};
    const uint64_t offset = (static_cast<uint64_t>(rect_y) * phys_w + rect_x) * 4;

    uint32_t resp = 0;

    GpuCmdTransfer2D xfer{};
    xfer.hdr.type = GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer.rect = last_rect_;
    xfer.offset = offset;
    xfer.resource_id = RESOURCE_ID;
    if (!submit_cmd(&xfer, sizeof(xfer), sizeof(GpuCmdHeader), &resp)) {
        ++commit_errors_;
    }

    GpuCmdResourceFlush flush{};
    flush.hdr.type = GPU_CMD_RESOURCE_FLUSH;
    flush.rect = last_rect_;
    flush.resource_id = RESOURCE_ID;
    if (!submit_cmd(&flush, sizeof(flush), sizeof(GpuCmdHeader), &resp)) {
        ++commit_errors_;
    }
}

} // namespace virtio
} // namespace auroraos
