#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../drivers/storage/flash_device.hpp"
#include "../../services/vfs/vfs_service.hpp"
#include "../../vfs/littlefs_vnode.hpp"
#include "../../vfs/photon_cache.hpp"
#include "../../vfs/procfs.hpp"
#include "../../vfs/ramfs.hpp"
#include "../../vfs/softbus.hpp"
#include "../../vfs/vfs.hpp"

using auroraos::vfs::VfsOpcode;
using auroraos::vfs::VfsReply;
using auroraos::vfs::VfsRequest;
using auroraos::vfs::VfsServer;

// =============================================================================
// UART Rx/Tx host test harness for vfs/softbus.cpp
// =============================================================================
namespace {
std::vector<char> g_uart_rx_queue;
size_t g_uart_rx_pos = 0;

void push_uart_rx(const std::string& bytes) {
    for (char c : bytes) {
        g_uart_rx_queue.push_back(c);
    }
}

void clear_uart_rx() {
    g_uart_rx_queue.clear();
    g_uart_rx_pos = 0;
}

void pump_softbus(SerialRpcBus& bus) {
    while (g_uart_rx_pos < g_uart_rx_queue.size()) {
        bus.poll();
    }
}
} // namespace

extern "C" {
void uart_putc(char /*c*/) {}

int uart_getc_nb(char* c) {
    if (!c || g_uart_rx_pos >= g_uart_rx_queue.size()) {
        return 0;
    }
    *c = g_uart_rx_queue[g_uart_rx_pos++];
    return 1;
}
}

// =============================================================================
// 1. RamFile P0/P1 Tests: capacity<=0, negative offset/len, sparse hole zeroing
// =============================================================================
TEST(VfsAuditRamFsTest, RejectsNegativeAndZeroConstructorCapacitySafely) {
    RamFile neg(-64);
    EXPECT_EQ(neg.get_capacity(), 0);
    EXPECT_EQ(neg.get_size(nullptr), 0);

    RamFile zero(0);
    EXPECT_EQ(zero.get_capacity(), 0);
    EXPECT_EQ(zero.get_size(nullptr), 0);
}

TEST(VfsAuditRamFsTest, RejectsNullBufNegativeOffsetAndNegativeLength) {
    RamFile ramfs(64);
    ASSERT_EQ(ramfs.write("hello", 5, 0, nullptr), 5);

    char buf[16] = {};
    EXPECT_EQ(ramfs.read(nullptr, 5, 0, nullptr), -1);
    EXPECT_EQ(ramfs.read(buf, -1, 0, nullptr), -1);
    EXPECT_EQ(ramfs.read(buf, 5, -1, nullptr), -1);
    EXPECT_EQ(ramfs.read(buf, 0, 0, nullptr), 0);

    EXPECT_EQ(ramfs.write(nullptr, 5, 0, nullptr), -1);
    EXPECT_EQ(ramfs.write("x", -1, 0, nullptr), -1);
    EXPECT_EQ(ramfs.write("x", 1, -1, nullptr), -1);
    EXPECT_EQ(ramfs.write("x", 0, 0, nullptr), 0);
}

TEST(VfsAuditRamFsTest, SparseWriteWithoutReallocZerosGapEvenAfterTruncate) {
    RamFile ramfs(64);
    ASSERT_EQ(ramfs.write("AAAABBBBCCCC", 12, 0, nullptr), 12);
    ASSERT_EQ(ramfs.truncate(4), 0);
    EXPECT_EQ(ramfs.get_size(nullptr), 4);

    // Write past file_size_ without crossing capacity_ (64)
    ASSERT_EQ(ramfs.write("DD", 2, 10, nullptr), 2);
    EXPECT_EQ(ramfs.get_size(nullptr), 12);

    char out[12];
    memset(out, 0x7F, sizeof(out));
    ASSERT_EQ(ramfs.read(out, 12, 0, nullptr), 12);
    EXPECT_EQ(memcmp(out, "AAAA", 4), 0);
    for (int i = 4; i < 10; ++i) {
        EXPECT_EQ(out[i], '\0') << "Sparse hole byte at index " << i << " leaked stale data";
    }
    EXPECT_EQ(memcmp(out + 10, "DD", 2), 0);
}

// =============================================================================
// 2. VfsManager & VfsServer P0 & VNode Reference Count P2 Tests
// =============================================================================
TEST(VfsAuditVfsManagerTest, GuardsNullAndNegativeParameters) {
    VfsManager& vfs = VfsManager::instance();
    RamFile file(64);
    ASSERT_TRUE(vfs.mount("/audit_ram", &file));

    EXPECT_EQ(vfs.open(nullptr, 0), -1);
    int fd = vfs.open("/audit_ram", 0);
    ASSERT_GE(fd, 0);

    char buf[16] = {};
    EXPECT_EQ(vfs.read(fd, nullptr, 4), -1);
    EXPECT_EQ(vfs.read(fd, buf, -1), -1);
    EXPECT_EQ(vfs.read(fd, buf, 0), 0);

    EXPECT_EQ(vfs.write(fd, nullptr, 4), -1);
    EXPECT_EQ(vfs.write(fd, "ab", -1), -1);
    EXPECT_EQ(vfs.write(fd, "ab", 0), 0);
    EXPECT_EQ(vfs.write(fd, "ab", 2), 2);

    EXPECT_EQ(vfs.close(fd), 0);
    EXPECT_TRUE(vfs.unmount("/audit_ram"));
}

TEST(VfsAuditVfsManagerTest, ServerRejectsNegativeReadWriteLengthDirectly) {
    VfsServer& srv = VfsServer::instance();
    RamFile file(64);
    ASSERT_TRUE(srv.mount("/audit_srv", &file));

    VfsRequest req{};
    VfsReply reply{};
    req.opcode = VfsOpcode::Open;
    strncpy(req.open.path, "/audit_srv", sizeof(req.open.path) - 1);
    srv.process_request(req, reply);
    ASSERT_GE(reply.status, 0);
    const int fd = reply.status;

    memset(&req, 0, sizeof(req));
    req.opcode = VfsOpcode::Write;
    req.fd = fd;
    req.write.len = -10;
    srv.process_request(req, reply);
    EXPECT_EQ(reply.status, -1);

    memset(&req, 0, sizeof(req));
    req.opcode = VfsOpcode::Read;
    req.fd = fd;
    req.read.len = -10;
    srv.process_request(req, reply);
    EXPECT_EQ(reply.status, -1);

    memset(&req, 0, sizeof(req));
    req.opcode = VfsOpcode::Close;
    req.fd = fd;
    srv.process_request(req, reply);
    EXPECT_TRUE(srv.unmount("/audit_srv"));
}

TEST(VfsAuditVfsManagerTest, VNodeRefCountClampsAtZero) {
    RamFile node(16);
    EXPECT_EQ(node.get_ref(), 0);
    node.release_ref();
    EXPECT_EQ(node.get_ref(), 0);
    node.add_ref();
    node.add_ref();
    EXPECT_EQ(node.get_ref(), 2);
    node.release_ref();
    EXPECT_EQ(node.get_ref(), 1);
    node.release_ref();
    EXPECT_EQ(node.get_ref(), 0);
}

// =============================================================================
// 3. ProcFS All 10 Nodes P0 Buffer Bounds + CapsNode P2 IrqGuard Snapshot Test
// =============================================================================
TEST(VfsAuditProcFsTest, AllNodesGuardZeroNegLenAndNullBuf) {
    MemInfoNode mem_node;
    CpuInfoNode cpu_node;
    TaskInfoNode task_node;
    LatencyNode lat_node;
    PowerNode pwr_node;
    NetNode net_node;
    SoftbusNode sb_node;
    UptimeNode up_node;
    IrqNode irq_node;
    CapsNode caps_node;

    ProcNode* nodes[] = {
        &mem_node, &cpu_node, &task_node, &lat_node, &pwr_node, &net_node, &sb_node, &up_node, &irq_node, &caps_node,
    };

    for (ProcNode* node : nodes) {
        char canary[2] = {0x5A, 0x3C};
        EXPECT_EQ(node->read(canary, 0, 0, nullptr), 0);
        EXPECT_EQ(canary[0], 0x5A);
        EXPECT_EQ(node->read(canary, -5, 0, nullptr), 0);
        EXPECT_EQ(canary[0], 0x5A);
        EXPECT_EQ(node->read(nullptr, 32, 0, nullptr), -1);

        char tiny = 0x5A;
        EXPECT_EQ(node->read(&tiny, 1, 0, nullptr), 0);
        EXPECT_EQ(tiny, '\0');

        char ok[16] = {};
        EXPECT_EQ(node->read(ok, sizeof(ok), -1, nullptr), -1);
    }
}

namespace {
int g_irq_restore_count = 0;

void count_irq_restore_hook() {
    ++g_irq_restore_count;
}
} // namespace

TEST(VfsAuditProcFsTest, CapsNodeSnapshotsPerTaskInsteadOfHoldingOneGlobalIrqLock) {
    auto& sched = Scheduler::instance();
    static uint32_t stack1[256];
    static uint32_t stack2[256];
    auto* t1 = sched.create_task([]() {}, stack1, sizeof(stack1));
    auto* t2 = sched.create_task([]() {}, stack2, sizeof(stack2));
    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);

    t1->security.cspace[0] = {auroraos::kernel::CapType::Endpoint, {true, true, false, 0}, 77, nullptr};

    g_irq_restore_count = 0;
    Arch::g_arch_test_interrupt_hook = count_irq_restore_hook;

    CapsNode caps;
    char buf[512] = {};
    int n = caps.read(buf, sizeof(buf), 0, nullptr);
    Arch::g_arch_test_interrupt_hook = nullptr;

    EXPECT_GT(n, 0);
    EXPECT_NE(strstr(buf, "Endpoint"), nullptr);
    EXPECT_NE(strstr(buf, "77"), nullptr);
    // At least 1 IrqGuard for task_count + 1 per task snapshot (>= 3 critical sections)
    EXPECT_GE(g_irq_restore_count, 3);
}

// =============================================================================
// 4. PhotonCacheLayer P1/P2 + LittleFsAdapter/LittleFsVNode P1 End-to-End Tests
// =============================================================================
TEST(VfsAuditStorageTest, PhotonCacheDoesNotCacheUninitializedDataOnReadFailure) {
    FlashBlockDevice dev("fault_read_flash", 4096, 16, 256);
    dev.erase_block(0);
    PhotonCacheLayer cache(dev);

    dev.set_fault_injection_for_test(true, false, false);
    uint8_t out[16];
    memset(out, 0x11, sizeof(out));
    EXPECT_EQ(cache.read(0, 0, out, sizeof(out)), -1);
    dev.set_fault_injection_for_test(false, false, false);

    // Must not have marked bogus stack data as valid; subsequent read fetches clean 0xFF flash
    EXPECT_EQ(cache.read(0, 0, out, sizeof(out)), static_cast<int>(sizeof(out)));
    for (uint8_t b : out) {
        EXPECT_EQ(b, 0xFF);
    }
}

TEST(VfsAuditStorageTest, PhotonCacheEraseFailurePreservesDirtyCache) {
    FlashBlockDevice dev("fault_erase_flash", 4096, 16, 256);
    dev.erase_block(0);
    PhotonCacheLayer cache(dev);

    const uint8_t payload[8] = {'A', 'U', 'R', 'O', 'R', 'A', 'O', 'S'};
    ASSERT_EQ(cache.write(0, 0, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));
    ASSERT_EQ(cache.get_dirty_count(), 1u);

    dev.set_fault_injection_for_test(false, false, true);
    EXPECT_EQ(cache.erase(0), -1);
    dev.set_fault_injection_for_test(false, false, false);

    EXPECT_EQ(cache.get_dirty_count(), 1u);
    uint8_t readback[8] = {};
    EXPECT_EQ(cache.read(0, 0, readback, sizeof(readback)), static_cast<int>(sizeof(readback)));
    EXPECT_EQ(memcmp(readback, payload, sizeof(payload)), 0);

    EXPECT_EQ(cache.erase(0), 0);
    EXPECT_EQ(cache.get_dirty_count(), 0u);
}

TEST(VfsAuditStorageTest, PhotonCacheHandlesTickWraparoundAndInvalidArgs) {
    FlashBlockDevice dev("lru_wrap_flash", 4096, 16, 256);
    for (uint32_t b = 0; b < 4; ++b) {
        dev.erase_block(b);
    }
    PhotonCacheLayer cache(dev);

    // Load 8 pages into 8-slot pool: pages 0..7 in block 0
    uint8_t byte_val = 0;
    for (uint32_t p = 0; p < 8; ++p) {
        uint8_t v = static_cast<uint8_t>(0x10 + p);
        ASSERT_EQ(cache.write(0, p * 256, &v, 1), 1);
    }

    // Force tick_counter_ near 32-bit wrap and access pages 1..7 (keeping page 0 LRU)
    cache.set_tick_counter_for_test(0xFFFFFFFAu);
    for (uint32_t p = 1; p < 8; ++p) {
        ASSERT_EQ(cache.read(0, p * 256, &byte_val, 1), 1);
        EXPECT_EQ(byte_val, static_cast<uint8_t>(0x10 + p));
    }
    EXPECT_LT(cache.get_tick_counter_for_test(), 0xFFFF0000u);

    // Write page 8 (block 1 offset 0) -> must evict oldest page 0, keeping page 7 cached (hit!)
    const uint32_t hits_before = cache.get_hit_count();
    uint8_t new_v = 0x77;
    ASSERT_EQ(cache.write(1, 0, &new_v, 1), 1);

    // Page 7 must still be a cache hit
    ASSERT_EQ(cache.read(0, 7 * 256, &byte_val, 1), 1);
    EXPECT_EQ(cache.get_hit_count(), hits_before + 1);
    // Page 0 was evicted cleanly to flash and is still readable
    ASSERT_EQ(cache.read(0, 0, &byte_val, 1), 1);
    EXPECT_EQ(byte_val, 0x10);

    // Invalid arg guards
    EXPECT_EQ(cache.read(0, 0, nullptr, 4), -1);
    EXPECT_EQ(cache.write(0, 0, nullptr, 4), -1);
    EXPECT_EQ(cache.read(0, 0, &byte_val, 0), 0);
    EXPECT_EQ(cache.write(0, 0, &byte_val, 0), 0);
    EXPECT_EQ(cache.read(0, 4096, &byte_val, 1), -1);
}

TEST(VfsAuditStorageTest, LittleFsMountReadWriteSeekAndUnmountedGuards) {
    FlashBlockDevice dev("lfs_test_flash", 4096, 16, 256);
    for (uint32_t b = 0; b < 16; ++b) {
        dev.erase_block(b);
    }
    PhotonCacheLayer cache(dev);
    LittleFsAdapter lfs_adapter(cache, 4096, 16);
    LittleFsVNode lfs_vnode(lfs_adapter);

    // Before mount: every operation must fail cleanly instead of UB on unmounted lfs_t
    void* priv = nullptr;
    EXPECT_FALSE(lfs_adapter.is_mounted());
    EXPECT_EQ(lfs_vnode.open_file("/cfg", O_CREAT | O_RDWR, &priv), -1);
    EXPECT_EQ(priv, nullptr);
    char dummy[8] = {};
    int sentinel = 1;
    EXPECT_EQ(lfs_vnode.read(dummy, 4, 0, &sentinel), -1);
    EXPECT_EQ(lfs_vnode.write("ab", 2, 0, &sentinel), -1);
    EXPECT_EQ(lfs_vnode.get_size(&sentinel), 0);

    // Format + mount: verifies read/prog/erase/sync bridges satisfy LittleFS 0/<0 contract
    ASSERT_TRUE(lfs_adapter.format());
    ASSERT_TRUE(lfs_adapter.mount());
    EXPECT_TRUE(lfs_adapter.is_mounted());

    ASSERT_EQ(lfs_vnode.open_file("/hello.txt", O_CREAT | O_RDWR | O_TRUNC, &priv), 0);
    ASSERT_NE(priv, nullptr);
    EXPECT_EQ(lfs_vnode.write("AuroraLFS!", 10, 0, priv), 10);
    EXPECT_EQ(lfs_vnode.get_size(priv), 10);
    EXPECT_EQ(lfs_vnode.close_file(priv), 0);

    void* rd_priv = nullptr;
    ASSERT_EQ(lfs_vnode.open_file("/hello.txt", O_RDONLY, &rd_priv), 0);
    ASSERT_NE(rd_priv, nullptr);

    char sub[5] = {};
    EXPECT_EQ(lfs_vnode.read(sub, 4, 6, rd_priv), 4);
    EXPECT_STREQ(sub, "LFS!");
    EXPECT_EQ(lfs_vnode.read(sub, 4, -1, rd_priv), -1);
    EXPECT_EQ(lfs_vnode.read(sub, -2, 0, rd_priv), -1);
    EXPECT_EQ(lfs_vnode.read(nullptr, 4, 0, rd_priv), -1);
    EXPECT_EQ(lfs_vnode.write("x", 1, -3, rd_priv), -1);

    // Unmount before close must free lfs_file_t safely without UB
    lfs_adapter.unmount();
    EXPECT_EQ(lfs_vnode.close_file(rd_priv), -1);
}

// =============================================================================
// 5. SerialRpcBus P2 State Machine & Auth Boundary Tests
// =============================================================================
namespace {
std::string g_last_rpc_payload;
int g_rpc_call_count = 0;

void test_rpc_handler(const char* payload) {
    ++g_rpc_call_count;
    g_last_rpc_payload = payload ? payload : "";
}
} // namespace

TEST(VfsAuditSoftBusTest, AuthRejectsKeyPrefixSpoofingAndResyncsOnDollarInAllStates) {
    SerialRpcBus& bus = SerialRpcBus::instance();
    ASSERT_TRUE(bus.register_service("PING", test_rpc_handler));
    ASSERT_TRUE(bus.register_service("TOO_LONG_COMMAN", test_rpc_handler));

    // 1. Key prefix forgery without ':' or ' ' delimiter must be rejected
    g_rpc_call_count = 0;
    g_last_rpc_payload.clear();
    clear_uart_rx();
    push_uart_rx("$RPC,PING,AURORA_RPC_KEY_FAKE_DATA#");
    push_uart_rx("$RPC,PING,AURORA_RPC_KEY2#");
    pump_softbus(bus);
    EXPECT_EQ(g_rpc_call_count, 0);

    // 2. Valid bare key and separated keys must pass with stripped prefix
    clear_uart_rx();
    push_uart_rx("$RPC,PING,AURORA_RPC_KEY#");
    pump_softbus(bus);
    EXPECT_EQ(g_rpc_call_count, 1);
    EXPECT_EQ(g_last_rpc_payload, "");

    clear_uart_rx();
    push_uart_rx("$RPC,PING,AURORA_RPC_KEY:hello#");
    pump_softbus(bus);
    EXPECT_EQ(g_rpc_call_count, 2);
    EXPECT_EQ(g_last_rpc_payload, "hello");

    // 3. Interrupted partial frames at states 1..6 + error discard state 7 must resync on '$'
    const char* partials[] = {
        "$", "$R", "$RP", "$RPC", "$RPC,PI", "$RPC,PING,mid_payload", "$RPC,TOO_LONG_COMMAND_NAME_IN_STATE_7,bad",
    };
    for (const char* p : partials) {
        const int before = g_rpc_call_count;
        clear_uart_rx();
        push_uart_rx(p);
        push_uart_rx("$RPC,PING,AURORA_RPC_KEY ok#");
        pump_softbus(bus);
        EXPECT_EQ(g_rpc_call_count, before + 1) << "Failed to resync after partial: " << p;
        EXPECT_EQ(g_last_rpc_payload, "ok");
    }

    // 4. Command > 15 bytes must transition to error state instead of truncating to 15 chars
    const int before_overlong = g_rpc_call_count;
    clear_uart_rx();
    push_uart_rx("$RPC,TOO_LONG_COMMAND_TRUNC,AURORA_RPC_KEY:evil#");
    pump_softbus(bus);
    EXPECT_EQ(g_rpc_call_count, before_overlong);

    // Exact 15-byte command continues working normally afterwards
    clear_uart_rx();
    push_uart_rx("$RPC,TOO_LONG_COMMAN,AURORA_RPC_KEY:good#");
    pump_softbus(bus);
    EXPECT_EQ(g_rpc_call_count, before_overlong + 1);
    EXPECT_EQ(g_last_rpc_payload, "good");
}
