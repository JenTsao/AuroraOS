// =============================================================================
// drivers/virtio/virtio_blk.cpp — virtio-blk 块设备驱动实现
// =============================================================================
#include "virtio_blk.hpp"
#include "virtio_mmio.hpp"
#include "board.h"

namespace auroraos {
namespace virtio {

VirtioBlk& VirtioBlk::instance() {
    // 板级常量定位 blk 槽位（QEMU -device 声明顺序即槽号，见 board.h）
    static VirtioMmioTransport mmio(BOARD_VIRTIO_MMIO_BASE +
                                    BOARD_VIRTIO_BLK_SLOT * BOARD_VIRTIO_MMIO_STRIDE);
    static VirtioBlk dev("vda", mmio);
    return dev;
}

void blk_build_chain(const BlkReqHeader* hdr, void* data, uint32_t data_len,
                     bool is_read, uint8_t* status, BufSeg out[3]) noexcept {
    out[0].addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hdr));
    out[0].len = sizeof(BlkReqHeader);
    out[0].device_writable = false;

    out[1].addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data));
    out[1].len = data_len;
    out[1].device_writable = is_read; // 读盘：设备写数据段

    out[2].addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(status));
    out[2].len = 1;
    out[2].device_writable = true;
}

bool blk_next_chunk(uint64_t byte_addr, uint32_t remaining,
                    uint32_t* sector, uint32_t* offset, uint32_t* chunk) noexcept {
    if (remaining == 0) {
        return false;
    }
    constexpr uint32_t kSector = VirtioBlk::SECTOR_SIZE;
    const uint32_t off = static_cast<uint32_t>(byte_addr % kSector);
    const uint32_t room = kSector - off;
    *sector = static_cast<uint32_t>(byte_addr / kSector);
    *offset = off;
    *chunk = remaining < room ? remaining : room;
    return true;
}

bool VirtioBlk::init() noexcept {
    if (!virtio_probe(transport_, DeviceId::Block)) {
        return false;
    }
    probed_ = true;

    virtio_reset(transport_);
    virtio_status_add(transport_, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    // blk 无必须 feature；VERSION_1 由协商层强制
    if (!virtio_negotiate_features(transport_, 0, 0, 0, 0, nullptr, nullptr) ||
        !virtio_features_ok(transport_)) {
        virtio_status_add(transport_, STATUS_FAILED);
        return false;
    }

    const uint16_t qsz =
        virtio_queue_configure(transport_, 0, QUEUE_SIZE, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(desc_)),
                               static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&avail_)),
                               static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&used_)));
    if (qsz == 0) {
        virtio_status_add(transport_, STATUS_FAILED);
        return false;
    }

    VirtQueue::Rings rings{desc_, &avail_, &used_};
    if (!queue_.init(rings, qsz, 0, transport_)) {
        virtio_status_add(transport_, STATUS_FAILED);
        return false;
    }

    // 配置空间偏移 0：容量（以 512B 扇区计，le64）
    const uint32_t cap_lo = transport_.read32(REG_CONFIG_SPACE + 0);
    const uint32_t cap_hi = transport_.read32(REG_CONFIG_SPACE + 4);
    capacity_sectors_ = (static_cast<uint64_t>(cap_hi) << 32) | cap_lo;
    if (capacity_sectors_ == 0) {
        virtio_status_add(transport_, STATUS_FAILED);
        return false;
    }

    virtio_driver_ok(transport_);
    ready_ = true;
    return true;
}

int VirtioBlk::issue(uint32_t type, uint64_t sector, void* data, bool data_device_writable) noexcept {
    req_hdr_.type = type;
    req_hdr_.reserved = 0;
    req_hdr_.sector = sector;
    req_status_ = 0xFF;

    BufSeg segs[3];
    blk_build_chain(&req_hdr_, data, SECTOR_SIZE, data_device_writable, &req_status_, segs);

    dma_sync_for_device(&req_hdr_, sizeof(req_hdr_));
    if (data) {
        dma_sync_for_device(data, SECTOR_SIZE);
    }
    dma_sync_for_device(&req_status_, sizeof(req_status_));

    const int head = queue_.add_buffer(segs, 3);
    if (head < 0) {
        return -1;
    }
    queue_.notify();

    // 自旋等待完成（轮询模式）。单请求在途：任何完成都必属本请求。
    uint32_t len = 0;
    for (uint32_t spin = 0; spin < MAX_POLL_SPINS; ++spin) {
        const int done_head = queue_.pop_used(&len);
        if (done_head >= 0) {
            if (done_head != head) {
                // 不应出现的孤儿完成：保守失败并停用设备（见 fail_device）
                fail_device();
                return -1;
            }
            dma_sync_for_cpu(&req_hdr_, sizeof(req_hdr_));
            if (data) {
                dma_sync_for_cpu(data, SECTOR_SIZE);
            }
            dma_sync_for_cpu(&req_status_, sizeof(req_status_));
            return req_status_ == BLK_S_OK ? 0 : -1;
        }
    }
    fail_device();
    return -1; // 超时：不挂死，交由调用方报错
}

void VirtioBlk::fail_device() noexcept {
    virtio_status_add(transport_, STATUS_FAILED);
    ready_ = false;
}

int VirtioBlk::xfer(bool is_write, uint64_t byte_addr, void* buf, uint32_t size) noexcept {
    uint32_t done = 0;
    while (done < size) {
        uint32_t sector = 0;
        uint32_t off = 0;
        uint32_t chunk = 0;
        blk_next_chunk(byte_addr + done, size - done, &sector, &off, &chunk);

        auto* p = static_cast<uint8_t*>(buf) + done;
        if (off == 0 && chunk == SECTOR_SIZE) {
            // 全扇区：设备 DMA 一律经内部 bounce_ 中转。请求超时后设备可能
            // 迟到回写，绝不能直接指向调用方缓冲（可能随栈帧销毁）。
            if (is_write) {
                for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
                    bounce_[i] = p[i];
                }
                if (issue(BLK_T_OUT, sector, bounce_, false) != 0) {
                    return -1;
                }
            } else {
                if (issue(BLK_T_IN, sector, bounce_, true) != 0) {
                    return -1;
                }
                for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
                    p[i] = bounce_[i];
                }
            }
        } else if (is_write) {
            // 部分扇区写：读-改-写
            if (issue(BLK_T_IN, sector, bounce_, true) != 0) {
                return -1;
            }
            for (uint32_t i = 0; i < chunk; ++i) {
                bounce_[off + i] = p[i];
            }
            if (issue(BLK_T_OUT, sector, bounce_, false) != 0) {
                return -1;
            }
        } else {
            // 部分扇区读：经 bounce 中转
            if (issue(BLK_T_IN, sector, bounce_, true) != 0) {
                return -1;
            }
            for (uint32_t i = 0; i < chunk; ++i) {
                p[i] = bounce_[off + i];
            }
        }
        done += chunk;
    }
    return 0;
}

int VirtioBlk::read_blocks(uint32_t block_addr, uint32_t offset, uint8_t* buf, uint32_t size) {
    // 溢出安全的边界检查（两条件形式，对齐 flash_device.hpp）
    if (!buf || block_addr >= capacity_sectors_ || offset > SECTOR_SIZE ||
        size > SECTOR_SIZE - offset) {
        return -1;
    }
    if (!ready_) {
        return -1;
    }

    LockGuard guard(io_mutex_);
    const uint64_t byte_addr = static_cast<uint64_t>(block_addr) * SECTOR_SIZE + offset;
    return xfer(false, byte_addr, buf, size);
}

int VirtioBlk::write_blocks(uint32_t block_addr, uint32_t offset, const uint8_t* buf, uint32_t size) {
    if (!buf || block_addr >= capacity_sectors_ || offset > SECTOR_SIZE ||
        size > SECTOR_SIZE - offset) {
        return -1;
    }
    if (!ready_) {
        return -1;
    }

    LockGuard guard(io_mutex_);
    const uint64_t byte_addr = static_cast<uint64_t>(block_addr) * SECTOR_SIZE + offset;
    return xfer(true, byte_addr, const_cast<uint8_t*>(buf), size);
}

} // namespace virtio
} // namespace auroraos
