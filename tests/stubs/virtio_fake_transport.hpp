// =============================================================================
// tests/stubs/virtio_fake_transport.hpp
//
// host 单元测试用的 virtio 传输假件：
//   - FakeTransport   — 普通内存模拟 virtio-mmio 寄存器文件
//   - HookedTransport — 在写 QueueNotify 时回调 notify_hook（模拟设备侧响应）
//   - VirtioBlkEmu    — 极简 virtio-blk 设备模拟器：消费 avail、按描述符链
//                       对镜像做读写、回填 used 与状态字节
// 仅测试链接，不进生产路径。
// =============================================================================
#ifndef AURORA_TESTS_VIRTIO_FAKE_TRANSPORT_HPP
#define AURORA_TESTS_VIRTIO_FAKE_TRANSPORT_HPP

#include <cstdint>
#include <cstring>

#include "../../drivers/virtio/virtio_defs.hpp"
#include "../../drivers/virtio/virtio_queue.hpp"
#include "../../drivers/virtio/virtio_transport.hpp"

namespace auroraos {
namespace virtio {
namespace testing {

class FakeTransport : public IVirtioTransport {
public:
    static constexpr uint32_t kRegBytes = 0x1000;
    static constexpr uint32_t kRegWords = kRegBytes / 4;

    uint32_t regs[kRegWords]{};

    // 设备侧 feature 字（按 DeviceFeaturesSel 复用返回）
    uint32_t dev_feat_lo = 0;
    uint32_t dev_feat_hi = 0; // bit0 = VIRTIO_F_VERSION_1

    uint32_t read32(uint32_t reg_offset) noexcept override {
        if (reg_offset == REG_DEVICE_FEATURES) {
            return (regs[REG_DEVICE_FEATURES_SEL / 4] & 1u) ? dev_feat_hi : dev_feat_lo;
        }
        if (reg_offset + 4 > kRegBytes) {
            return 0;
        }
        uint32_t v = 0;
        std::memcpy(&v, &regs[reg_offset / 4], sizeof(v));
        return v;
    }

    void write32(uint32_t reg_offset, uint32_t value) noexcept override {
        if (reg_offset + 4 > kRegBytes) {
            return;
        }
        std::memcpy(&regs[reg_offset / 4], &value, sizeof(value));
    }
};

// 写 QueueNotify 触发回调（模拟设备被敲门铃），回调带队列号
class HookedTransport : public FakeTransport {
public:
    using NotifyHook = void (*)(void* ctx, uint32_t queue_idx);
    NotifyHook notify_hook = nullptr;
    void* notify_ctx = nullptr;

    void write32(uint32_t reg_offset, uint32_t value) noexcept override {
        FakeTransport::write32(reg_offset, value);
        if (reg_offset == REG_QUEUE_NOTIFY && notify_hook) {
            notify_hook(notify_ctx, value);
        }
    }
};

// 极简 virtio-blk 设备模拟器（单队列、顺序完成）
class VirtioBlkEmu {
public:
    VirtqDesc* desc = nullptr;
    AvailRing* avail = nullptr;
    UsedRing* used = nullptr;
    uint16_t qsize = 0;

    uint8_t* image = nullptr; // 镜像（字节）
    uint64_t image_bytes = 0;

    uint8_t forced_status = BLK_S_OK; // 错误注入：回填到请求状态字节
    int request_count = 0;

    void bind(VirtqDesc* d, AvailRing* a, UsedRing* u, uint16_t size) {
        desc = d;
        avail = a;
        used = u;
        qsize = size;
    }

    // 可直接作为 HookedTransport::notify_hook 使用
    static void on_notify_trampoline(void* ctx, uint32_t /*queue_idx*/) {
        static_cast<VirtioBlkEmu*>(ctx)->on_notify();
    }

    void on_notify() {
        if (!desc || !avail || !used) {
            return; // init 早期 notify（尚未 bind）不动作
        }
        while (consumed_ != avail->idx) {
            const uint16_t head = avail->ring[consumed_ % qsize];
            consumed_ = static_cast<uint16_t>(consumed_ + 1);
            complete_one(head);
        }
    }

private:
    uint16_t consumed_ = 0;

    void complete_one(uint16_t head) {
        ++request_count;

        // 走描述符链：[hdr(16B)][data][status(1B)]
        uint16_t idx = head;
        BlkReqHeader hdr{};
        std::memcpy(&hdr, reinterpret_cast<const void*>(static_cast<uintptr_t>(desc[idx].addr)),
                    sizeof(hdr));
        const bool chain_ok_0 = (desc[idx].flags & VRING_DESC_F_NEXT) != 0;
        idx = desc[idx].next;

        void* data = reinterpret_cast<void*>(static_cast<uintptr_t>(desc[idx].addr));
        const uint32_t data_len = desc[idx].len;
        const bool chain_ok_1 = (desc[idx].flags & VRING_DESC_F_NEXT) != 0;
        idx = desc[idx].next;

        uint8_t* status = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(desc[idx].addr));
        const bool chain_ok_2 = (desc[idx].flags & VRING_DESC_F_NEXT) == 0;

        uint8_t st = forced_status;
        if (!chain_ok_0 || !chain_ok_1 || !chain_ok_2) {
            st = BLK_S_IOERR; // 链形状不对
        } else if (hdr.type == BLK_T_IN || hdr.type == BLK_T_OUT) {
            const uint64_t off = hdr.sector * 512;
            if (image && data_len <= image_bytes && off + data_len <= image_bytes) {
                if (hdr.type == BLK_T_IN) {
                    std::memcpy(data, image + off, data_len);
                } else {
                    std::memcpy(image + off, data, data_len);
                }
            } else {
                st = BLK_S_IOERR;
            }
        } else if (hdr.type != BLK_T_FLUSH) {
            st = BLK_S_UNSUPP;
        }

        if (status) {
            *status = st;
        }

        used->ring[used->idx % qsize] = VirtqUsedElem{head, 0};
        used->idx = static_cast<uint16_t>(used->idx + 1);
    }
};

// 极简 virtio-gpu 控制队列模拟器：记录命令、回填成功响应
class VirtioGpuEmu {
public:
    VirtqDesc* desc = nullptr;
    AvailRing* avail = nullptr;
    UsedRing* used = nullptr;
    uint16_t qsize = 0;

    static constexpr int kMaxLog = 64;
    uint32_t cmd_types[kMaxLog]{};
    uint32_t cmd_count = 0;
    int transfer_count = 0;
    int flush_count = 0;
    GpuRect last_transfer_rect{};
    uint64_t last_transfer_offset = 0;
    uint64_t last_attach_addr = 0;
    uint32_t last_attach_len = 0;
    uint32_t first_cmd_status = 0;
    const FakeTransport* transport = nullptr;

    void bind(VirtqDesc* d, AvailRing* a, UsedRing* u, uint16_t size) {
        desc = d;
        avail = a;
        used = u;
        qsize = size;
    }

    static void on_notify_trampoline(void* ctx, uint32_t /*queue_idx*/) {
        static_cast<VirtioGpuEmu*>(ctx)->on_notify();
    }

    void on_notify() {
        if (!desc || !avail || !used) {
            return; // init 早期 notify（尚未 bind）不动作
        }
        while (consumed_ != avail->idx) {
            const uint16_t head = avail->ring[consumed_ % qsize];
            consumed_ = static_cast<uint16_t>(consumed_ + 1);
            complete_one(head);
        }
    }

private:
    uint16_t consumed_ = 0;

    void complete_one(uint16_t head) {
        // 链 [cmd(设备只读)][resp(设备写)]
        uint16_t idx = head;
        const void* cmd = reinterpret_cast<const void*>(static_cast<uintptr_t>(desc[idx].addr));
        const bool chain_ok_0 = (desc[idx].flags & VRING_DESC_F_NEXT) != 0;
        idx = desc[idx].next;
        void* resp = reinterpret_cast<void*>(static_cast<uintptr_t>(desc[idx].addr));
        const bool chain_ok_1 = (desc[idx].flags & VRING_DESC_F_NEXT) == 0;

        uint32_t type = 0;
        if (cmd) {
            std::memcpy(&type, cmd, sizeof(type));
        }
        if (cmd_count == 0 && transport) {
            first_cmd_status = transport->regs[REG_STATUS / 4];
        }
        if (cmd_count < kMaxLog) {
            cmd_types[cmd_count] = type;
        }
        ++cmd_count;

        if (type == GPU_CMD_RESOURCE_ATTACH_BACKING) {
            GpuCmdAttachBacking a{};
            std::memcpy(&a, cmd, sizeof(a));
            last_attach_addr = a.entries[0].mem_offset;
            last_attach_len = a.entries[0].length;
        } else if (type == GPU_CMD_TRANSFER_TO_HOST_2D) {
            GpuCmdTransfer2D x{};
            std::memcpy(&x, cmd, sizeof(x));
            last_transfer_rect = x.rect;
            last_transfer_offset = x.offset;
            ++transfer_count;
        } else if (type == GPU_CMD_RESOURCE_FLUSH) {
            ++flush_count;
        }

        uint32_t resp_type = GPU_RESP_OK_NODATA;
        if (type == GPU_CMD_GET_DISPLAY_INFO) {
            resp_type = GPU_RESP_OK_DISPLAY_INFO;
        }
        GpuCmdHeader rh{};
        rh.type = (chain_ok_0 && chain_ok_1) ? resp_type : GPU_RESP_ERR_UNSPEC;
        if (resp) {
            std::memcpy(resp, &rh, sizeof(rh));
            if (type == GPU_CMD_GET_DISPLAY_INFO) {
                GpuRespDisplayOne one{};
                one.rect = GpuRect{0, 0, 640, 480};
                one.enabled = 1;
                std::memcpy(static_cast<uint8_t*>(resp) + sizeof(GpuCmdHeader), &one, sizeof(one));
            }
        }

        used->ring[used->idx % qsize] = VirtqUsedElem{head, 0};
        used->idx = static_cast<uint16_t>(used->idx + 1);
    }
};

// 极简 virtio-net 设备模拟器：完成 TX 请求、按需投递 RX 帧
class VirtioNetEmu {
public:
    VirtqDesc* rx_desc = nullptr;
    AvailRing* rx_avail = nullptr;
    UsedRing* rx_used = nullptr;
    VirtqDesc* tx_desc = nullptr;
    AvailRing* tx_avail = nullptr;
    UsedRing* tx_used = nullptr;
    uint16_t qsize = 0;

    int tx_count = 0;
    uint8_t last_tx_frame[1600]{};
    int last_tx_len = 0;

    void bind_rx(VirtqDesc* d, AvailRing* a, UsedRing* u, uint16_t size) {
        rx_desc = d;
        rx_avail = a;
        rx_used = u;
        qsize = size;
    }

    void bind_tx(VirtqDesc* d, AvailRing* a, UsedRing* u, uint16_t size) {
        tx_desc = d;
        tx_avail = a;
        tx_used = u;
        qsize = size;
    }

    static void on_notify_trampoline(void* ctx, uint32_t queue_idx) {
        static_cast<VirtioNetEmu*>(ctx)->on_notify(queue_idx);
    }

    void on_notify(uint32_t queue_idx) {
        if (queue_idx != 1 || !tx_desc || !tx_avail || !tx_used) {
            return; // RX 队列的投递缓冲只在来帧时消费
        }
        while (tx_consumed_ != tx_avail->idx) {
            const uint16_t head = tx_avail->ring[tx_consumed_ % qsize];
            tx_consumed_ = static_cast<uint16_t>(tx_consumed_ + 1);

            // 链 [NetHdr(10)][frame]
            uint16_t idx = head;
            const bool chain_ok_0 = (tx_desc[idx].flags & VRING_DESC_F_NEXT) != 0;
            idx = tx_desc[idx].next;
            const void* frame = reinterpret_cast<const void*>(static_cast<uintptr_t>(tx_desc[idx].addr));
            const uint32_t len = tx_desc[idx].len;

            ++tx_count;
            if (chain_ok_0 && frame && len <= sizeof(last_tx_frame)) {
                std::memcpy(last_tx_frame, frame, len);
                last_tx_len = static_cast<int>(len);
            } else {
                last_tx_len = -1;
            }

            tx_used->ring[tx_used->idx % qsize] = VirtqUsedElem{head, 0};
            tx_used->idx = static_cast<uint16_t>(tx_used->idx + 1);
        }
    }

    // 设备侧来帧：取一个已投递 RX 缓冲（单描述符：NetHdr+帧连续区），
    // 写入 NetHdr + 帧，回填 used
    bool deliver_rx(const uint8_t* frame, int len) {
        if (!rx_desc || !rx_avail || !rx_used || rx_consumed_ == rx_avail->idx) {
            return false;
        }
        const uint16_t head = rx_avail->ring[rx_consumed_ % qsize];
        rx_consumed_ = static_cast<uint16_t>(rx_consumed_ + 1);

        uint8_t* base = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(rx_desc[head].addr));
        const uint32_t total = rx_desc[head].len;
        if (!base || total <= 10) {
            return false;
        }
        std::memset(base, 0, 10);
        const uint32_t cap = total - 10;
        const int n = len < static_cast<int>(cap) ? len : static_cast<int>(cap);
        std::memcpy(base + 10, frame, n);

        rx_used->ring[rx_used->idx % qsize] = VirtqUsedElem{head, static_cast<uint32_t>(10 + n)};
        rx_used->idx = static_cast<uint16_t>(rx_used->idx + 1);
        return true;
    }

private:
    uint16_t tx_consumed_ = 0;
    uint16_t rx_consumed_ = 0;
};

} // namespace testing
} // namespace virtio
} // namespace auroraos

#endif // AURORA_TESTS_VIRTIO_FAKE_TRANSPORT_HPP
