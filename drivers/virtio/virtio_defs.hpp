// =============================================================================
// drivers/virtio/virtio_defs.hpp
//
// virtio-mmio 传输寄存器偏移、状态/feature 位与 split virtqueue / blk / net /
// gpu 打包结构体。本头文件零依赖（仅 <stdint.h>），是 virtio 栈的纯常量层：
// host 单元测试与固件共享同一份定义。
//
// 参考规范：OASIS virtio 1.2（virtio-mmio transport、split virtqueue）。
// 仅支持 modern（Version==2）设备，不实现 legacy（Version==1）布局。
// =============================================================================
#ifndef AURORA_VIRTIO_DEFS_HPP
#define AURORA_VIRTIO_DEFS_HPP

#include <stdint.h>

namespace auroraos {
namespace virtio {

// ---------------------------------------------------------------------------
// virtio-mmio 寄存器偏移（32-bit 访问，字对齐）
// ---------------------------------------------------------------------------
constexpr uint32_t REG_MAGIC_VALUE = 0x000;      // 只读，应为 0x74726976 ("virt")
constexpr uint32_t REG_VERSION = 0x004;          // 只读，2 = modern
constexpr uint32_t REG_DEVICE_ID = 0x008;        // 只读，0 = 不存在
constexpr uint32_t REG_VENDOR_ID = 0x00c;
constexpr uint32_t REG_DEVICE_FEATURES = 0x010;  // 读：DeviceFeaturesSel 选中的 32 位
constexpr uint32_t REG_DEVICE_FEATURES_SEL = 0x014;
constexpr uint32_t REG_DRIVER_FEATURES = 0x020;  // 写：DriverFeaturesSel 选中的 32 位
constexpr uint32_t REG_DRIVER_FEATURES_SEL = 0x024;
constexpr uint32_t REG_QUEUE_SEL = 0x030;
constexpr uint32_t REG_QUEUE_NUM_MAX = 0x034;    // 只读，0 = 队列不存在
constexpr uint32_t REG_QUEUE_NUM = 0x038;
constexpr uint32_t REG_QUEUE_READY = 0x044;
constexpr uint32_t REG_QUEUE_NOTIFY = 0x050;
constexpr uint32_t REG_INTERRUPT_STATUS = 0x060;
constexpr uint32_t REG_INTERRUPT_ACK = 0x064;
constexpr uint32_t REG_STATUS = 0x070;
constexpr uint32_t REG_QUEUE_DESC_LOW = 0x080;
constexpr uint32_t REG_QUEUE_DESC_HIGH = 0x084;
// avail/used 环地址寄存器必须与 Linux include/standard-headers/linux/
// virtio_mmio.h（QEMU 同源）逐偏移一致：AVAIL=0x090/0x094，USED=0x0a0/0x0a4。
// 此前误写为 0x088/0x08c 与 0x090/0x094 —— avail 地址落进空洞被设备忽略、
// used 地址占住 AVAIL 寄存器，设备把 used 环当 avail 环读，avail->idx 恒 0，
// 所有请求静默无完成（fake transport 直接绑环不经寄存器，单测无法覆盖）。
constexpr uint32_t REG_QUEUE_DRIVER_LOW = 0x090; // avail ring（driver-owned）
constexpr uint32_t REG_QUEUE_DRIVER_HIGH = 0x094;
constexpr uint32_t REG_QUEUE_DEVICE_LOW = 0x0a0; // used ring（device-owned）
constexpr uint32_t REG_QUEUE_DEVICE_HIGH = 0x0a4;
constexpr uint32_t REG_CONFIG_GENERATION = 0x0fc;
constexpr uint32_t REG_CONFIG_SPACE = 0x100;     // 设备配置空间起始

constexpr uint32_t VIRTIO_MMIO_MAGIC = 0x74726976u;
constexpr uint32_t VIRTIO_MMIO_VERSION_MODERN = 2;

// ---------------------------------------------------------------------------
// Status 寄存器位（驱动按 ACKNOWLEDGE→DRIVER→FEATURES_OK→DRIVER_OK 顺序置位）
// ---------------------------------------------------------------------------
constexpr uint32_t STATUS_ACKNOWLEDGE = 1;
constexpr uint32_t STATUS_DRIVER = 2;
constexpr uint32_t STATUS_DRIVER_OK = 4;
constexpr uint32_t STATUS_FEATURES_OK = 8;
constexpr uint32_t STATUS_DEVICE_NEEDS_RESET = 64;
constexpr uint32_t STATUS_FAILED = 128;

// ---------------------------------------------------------------------------
// 通用 feature 位（编号 32+ 落在 DeviceFeaturesSel=1 的高 32 位窗口）
// ---------------------------------------------------------------------------
constexpr uint32_t F_VERSION_1_BIT = 32;        // 必须协商（modern 契约）
constexpr uint32_t F_ACCESS_PLATFORM_BIT = 33;  // 绝不协商（本平台无 IOMMU）
constexpr uint32_t F_RING_INDIRECT_DESC_BIT = 28;
constexpr uint32_t F_RING_EVENT_IDX_BIT = 29;

// 设备专属 feature 位（低 32 位窗口）
constexpr uint32_t NET_F_MAC_BIT = 5;
constexpr uint32_t NET_F_MRG_RXBUF_BIT = 15;

// ---------------------------------------------------------------------------
// 设备类型（DeviceID 寄存器）
// ---------------------------------------------------------------------------
enum class DeviceId : uint32_t {
    None = 0,
    Net = 1,
    Block = 2,
    Console = 3,
    Gpu = 16,
};

// ---------------------------------------------------------------------------
// split virtqueue 结构（modern：desc / avail / used 三段独立寻址）
// ---------------------------------------------------------------------------

// 可用环元素的缓冲段描述（驱动侧组装描述符链的输入）
struct BufSeg {
    uint64_t addr;  // 缓冲物理地址（host 测试中即宿主指针）
    uint32_t len;   // 字节数
    bool device_writable; // true → 描述符标记 VRING_DESC_F_WRITE
};

constexpr uint16_t VRING_DESC_F_NEXT = 1;
constexpr uint16_t VRING_DESC_F_WRITE = 2;

struct VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
static_assert(sizeof(VirtqDesc) == 16, "virtq desc must be 16 bytes");

struct VirtqAvail {
    uint16_t flags;
    uint16_t idx;
    // ring[] 与 used_event 由 VirtQueue 的缓冲布局决定大小（QSIZE 可变），
    // 此处仅定义头部形状；ring 通过 VirtQueue::Rings 指针按 QSIZE 索引。
};
static_assert(sizeof(VirtqAvail) == 4, "virtq avail header must be 4 bytes");

struct VirtqUsedElem {
    uint32_t id; // 链头描述符下标
    uint32_t len; // 设备写入的总字节数
};
static_assert(sizeof(VirtqUsedElem) == 8, "virtq used elem must be 8 bytes");

struct VirtqUsed {
    uint16_t flags;
    uint16_t idx;
    // ring[] 紧随其后，元素类型 VirtqUsedElem。
};

// ---------------------------------------------------------------------------
// virtio-blk 请求结构
// ---------------------------------------------------------------------------
constexpr uint32_t BLK_T_IN = 0;  // 设备→内存（读盘）
constexpr uint32_t BLK_T_OUT = 1; // 内存→设备（写盘）
constexpr uint32_t BLK_T_FLUSH = 4;

constexpr uint8_t BLK_S_OK = 0;
constexpr uint8_t BLK_S_IOERR = 1;
constexpr uint8_t BLK_S_UNSUPP = 2;

struct BlkReqHeader {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};
static_assert(sizeof(BlkReqHeader) == 16, "blk req header must be 16 bytes");

// ---------------------------------------------------------------------------
// virtio-net 头（VirtIO 1.0+ Modern 规范固定为 12 字节，含 num_buffers）
// ---------------------------------------------------------------------------
struct NetHdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};
static_assert(sizeof(NetHdr) == 12, "virtio-net modern hdr must be 12 bytes");

// ---------------------------------------------------------------------------
// virtio-gpu 2D 命令结构
// ---------------------------------------------------------------------------
constexpr uint32_t GPU_CMD_GET_DISPLAY_INFO = 0x0100;
constexpr uint32_t GPU_CMD_RESOURCE_CREATE_2D = 0x0101;
constexpr uint32_t GPU_CMD_RESOURCE_UNREF = 0x0102;
constexpr uint32_t GPU_CMD_SET_SCANOUT = 0x0103;
constexpr uint32_t GPU_CMD_RESOURCE_FLUSH = 0x0104;
constexpr uint32_t GPU_CMD_TRANSFER_TO_HOST_2D = 0x0105;
constexpr uint32_t GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;
constexpr uint32_t GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107;

constexpr uint32_t GPU_RESP_OK_NODATA = 0x1100;
constexpr uint32_t GPU_RESP_OK_DISPLAY_INFO = 0x1101;
constexpr uint32_t GPU_RESP_ERR_UNSPEC = 0x1200;
constexpr uint32_t GPU_RESP_ERR_OUT_OF_MEMORY = 0x1201;
constexpr uint32_t GPU_RESP_ERR_SCANOUT_ID = 0x1202;
constexpr uint32_t GPU_RESP_ERR_INVALID_RESOURCE_ID = 0x1203;
constexpr uint32_t GPU_RESP_ERR_INVALID_PARAMETER = 0x1205;

// 像素格式（RESOURCE_CREATE_2D.format）
constexpr uint32_t GPU_FORMAT_B8G8R8X8_UNORM = 1;

// 所有 gpu 命令共享 24 字节头；响应头形状相同（type 换成 resp 码）。
struct GpuCmdHeader {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};
static_assert(sizeof(GpuCmdHeader) == 24, "gpu cmd header must be 24 bytes");

struct GpuRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(GpuRect) == 16, "gpu rect must be 16 bytes");

struct GpuRespDisplayOne {
    GpuRect rect;
    uint32_t enabled;
    uint32_t flags;
};
static_assert(sizeof(GpuRespDisplayOne) == 24, "gpu display one must be 24 bytes");

constexpr uint32_t GPU_MAX_SCANOUTS = 16;

// GET_DISPLAY_INFO 响应 = 头(24) + 16 * GpuRespDisplayOne(24) = 384 字节
constexpr uint32_t GPU_DISPLAY_INFO_RESP_SIZE = sizeof(GpuCmdHeader) + GPU_MAX_SCANOUTS * sizeof(GpuRespDisplayOne);

// 命令载荷（头 + 参数，直接作为描述符链第一段提交）
struct GpuCmdCreate2D {
    GpuCmdHeader hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

// RESOURCE_UNREF：释放资源（初始化失败回滚用）
struct GpuCmdResourceUnref {
    GpuCmdHeader hdr;
    uint32_t resource_id;
};

struct GpuMemEntry {
    uint64_t mem_offset;
    uint32_t length;
    uint32_t padding;
};

struct GpuCmdAttachBacking {
    GpuCmdHeader hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    GpuMemEntry entries[1];
};

struct GpuCmdSetScanout {
    GpuCmdHeader hdr;
    GpuRect rect;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct GpuCmdTransfer2D {
    GpuCmdHeader hdr;
    GpuRect rect;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct GpuCmdResourceFlush {
    GpuCmdHeader hdr;
    GpuRect rect;
    uint32_t resource_id;
    uint32_t padding;
};

} // namespace virtio
} // namespace auroraos

#endif // AURORA_VIRTIO_DEFS_HPP
