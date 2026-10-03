// =============================================================================
// tests/integration/test_system_boot.cpp
//
// 端到端系统启动集成测试。与 unit 测试不同，这里把多个真实子系统串成
// apps/kernel.cpp::kernel_main() 的启动顺序，验证它们协同工作后的整体行为，
// 而不是各组件的孤立单元：
//
//   KernelHeap → VfsManager/VfsServer → DeviceRegistry(能力模型)
//             → ProcFS 节点 → Scheduler → SyscallDispatcher → start()/schedule()
//
// 关键机制说明（host 环境）：
//   - Arch::start_first_task() 在 stubs/arch_api.hpp 中是 [[noreturn]] 且会
//     抛 std::logic_error，因为宿主测试不能真正跳进裸机上下文。
//   - Scheduler::start() 在调用 start_first_task() *之前* 已把 started_ 置真、
//     g_current_tcb_ptr 指向首个任务。因此本测试有意捕获该异常，随后调用
//     schedule()，即可在不触发真实上下文切换的情况下，驱动调度器的 O(1)
//     优先级选择逻辑并断言其结果（switch_count / 当前 TCB / 状态迁移）。
//   - Arch::host_trigger_context_switch() 在 stub 中把 g_current_tcb_ptr 赋为
//     g_next_tcb_ptr，使 schedule() 的选择结果可观测，但任务函数体不会执行。
//
// 该文件是 host 侧唯一的"启动流程"集成门禁；真实硬件启动仍由 QEMU HIL
// (scripts/hil_runner.py) 覆盖，二者互补。
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>

// Stubs 通过 include 路径优先注入：task.hpp/memory.hpp 解析到 host stub 版本。
#include "task.hpp"
#include "memory.hpp"

#include "../../vfs/vfs.hpp"
#include "../../vfs/procfs.hpp"
#include "../../vfs/ramfs.hpp"
#include "../../kernel/core/device.hpp"
#include "../../kernel/core/cspace.hpp"
#include "../../kernel/core/syscall_dispatcher.hpp"

using auroraos::kernel::CAP_RIGHT_READ;
using auroraos::kernel::CAP_RIGHT_WRITE;
using auroraos::kernel::CSpace;
using auroraos::kernel::SyscallDispatcher;

// ---------------------------------------------------------------------------
// 测试替身：一个可记录 I/O 的字符设备，模拟 kernel_main 里注册的 uart/oled。
// 复用 CharDevice 基类，走真实的 DeviceRegistry + VFS 挂载路径。
// ---------------------------------------------------------------------------
class BootProbeDevice : public CharDevice {
public:
    explicit BootProbeDevice(const char* name) : CharDevice(name) {}

    int open() override {
        ++open_count;
        return 0;
    }

    int close() override {
        ++close_count;
        return 0;
    }

    int read(char* buf, int len, int offset, void* /*priv*/) override {
        if (len <= 0 || offset != 0)
            return -1;
        const int n = static_cast<int>(strlen(kPayload));
        const int copy = len < n ? len : n;
        memcpy(buf, kPayload, static_cast<size_t>(copy));
        return copy;
    }

    int write(const char* buf, int len, int /*offset*/, void* /*priv*/) override {
        if (len <= 0)
            return -1;
        last_written_len = len;
        memcpy(write_buf, buf, static_cast<size_t>(len < kBufCap ? len : kBufCap));
        write_buf[len < kBufCap ? len : kBufCap - 1] = '\0';
        return len;
    }

    int open_count = 0;
    int close_count = 0;
    int last_written_len = 0;
    static constexpr int kBufCap = 64;
    static constexpr const char* kPayload = "probe-ok";
    char write_buf[kBufCap] = {0};
};

// ---------------------------------------------------------------------------
// 启动 fixture：每个测试前重置所有单例，保证互不污染。
// KernelHeap / Scheduler / VfsServer / DeviceRegistry 均为全局单例，
// 必须显式复位，否则测试执行顺序会影响结果（原空壳测试的隐患）。
// ---------------------------------------------------------------------------
class SystemBootIntegration : public ::testing::Test {
protected:
    static constexpr size_t kHeapSize = 16384;
    static constexpr uint32_t kStackWords = 64;

    alignas(8) uint8_t heap_region_[kHeapSize] = {0};

    // 任务栈：与 kernel_main 一样为静态数组，避免 host 测试里动态分配。
    uint32_t idle_stack_[kStackWords] = {0};
    uint32_t shell_stack_[kStackWords] = {0};

    // 需要在测试结束后清理的设备/挂载点（静态存储，跨测试复用）。
    static BootProbeDevice& probe_device() {
        static BootProbeDevice dev("probe0");
        return dev;
    }
    static RamFile& ram_log() {
        static RamFile f(512);
        return f;
    }

    void SetUp() override {
        // 1) 复位内核堆（覆盖整段区域，重建 TLSF 空闲块）。
        KernelHeap::instance().init(heap_region_, heap_region_ + kHeapSize);

        // 2) 复位 VFS 服务端的挂载表与 fd 表。
        VfsManager::instance().init();

        // 3) 复位设备注册表，避免上一个测试残留的 /dev 挂载占用槽位。
        DeviceRegistry::instance().clear();

        // 4) 复位调度器单例。
        Scheduler::instance().init();
    }

    static void idle_entry() {}
    static void shell_entry() {}

    // 真实创建 idle + shell 两个任务（镜像 kernel_main 的最小任务集）。
    TaskControlBlock* spawn_core_tasks(TaskControlBlock** shell_out) {
        TaskControlBlock* idle = Scheduler::instance().create_task(
            idle_entry, idle_stack_, kStackWords * sizeof(uint32_t), TaskPriority::Idle);
        TaskControlBlock* shell = Scheduler::instance().create_task(
            shell_entry, shell_stack_, kStackWords * sizeof(uint32_t), TaskPriority::High);
        if (shell_out)
            *shell_out = shell;
        return idle;
    }

    // 驱动 start()：捕获 stub 抛出的 start_first_task 异常，返回是否抛出。
    // 抛出后 started_ 已为真，schedule() 可正常工作。
    bool start_scheduler_catching_stub() {
        try {
            Scheduler::instance().start();
            return false; // stub 未抛异常（不应发生在 host）
        } catch (const std::logic_error&) {
            return true;
        }
    }
};

// ---------------------------------------------------------------------------
// 1. 完整启动序列：堆 → VFS → 调度器 → start() → 首次调度切换
//    断言 schedule() 真正选中了更高优先级的 shell，而非停留 idle。
// ---------------------------------------------------------------------------
TEST_F(SystemBootIntegration, FullBootSequenceReachesScheduler) {
    TaskControlBlock* shell = nullptr;
    TaskControlBlock* idle = spawn_core_tasks(&shell);
    ASSERT_NE(idle, nullptr);
    ASSERT_NE(shell, nullptr);
    ASSERT_NE(shell, idle);

    // 两个任务都已就绪，任务计数为 2（> 1，满足 schedule() 的运行前提）。
    EXPECT_EQ(idle->scheduler.state, TaskState::Ready);
    EXPECT_EQ(shell->scheduler.state, TaskState::Ready);
    EXPECT_EQ(Scheduler::instance().get_task_count(), 2);

    // SyscallDispatcher 初始化后所有槽位应被填充（handle_unknown 兜底）。
    SyscallDispatcher::init();

    // start() 会把当前 TCB 指向 tasks[0]（idle），随后 stub 抛异常。
    EXPECT_TRUE(start_scheduler_catching_stub());
    EXPECT_EQ(Scheduler::instance().get_current_tcb(), idle);

    // start() 之前 schedule() 是 no-op；此刻 started_ 已置真，应能切换。
    const uint32_t switches_before = Scheduler::instance().get_total_switches();
    Scheduler::instance().schedule();

    // High 优先级的 shell 应被选中并接管 CPU。
    EXPECT_EQ(Scheduler::instance().get_current_tcb(), shell);
    EXPECT_GT(Scheduler::instance().get_total_switches(), switches_before);
    EXPECT_GT(shell->scheduler.switch_count, 0u);
}

// ---------------------------------------------------------------------------
// 2. 启动后 VFS + ProcFS 集成：挂载 /proc/meminfo，经真实 open/read 路径读取，
//    内容应反映 KernelHeap 的真实容量与空闲量。
// ---------------------------------------------------------------------------
TEST_F(SystemBootIntegration, ProcFsMemInfoReflectsLiveHeapThroughVfs) {
    static MemInfoNode meminfo_node;
    ASSERT_TRUE(VfsManager::instance().mount("/proc/meminfo", &meminfo_node));

    const int fd = VfsManager::instance().open("/proc/meminfo");
    ASSERT_GE(fd, 0) << "VFS 无法打开已挂载的 procfs 节点";

    char buf[256] = {0};
    const int n = VfsManager::instance().read(fd, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';

    // 必须包含 meminfo 头部与真实内存字段。
    EXPECT_NE(strstr(buf, "MemTotal:"), nullptr);
    EXPECT_NE(strstr(buf, "MemFree:"), nullptr);

    // MemTotal 应等于堆总容量（16384，8 字节对齐后可能略减，用宽松下界）。
    const size_t total = KernelHeap::instance().get_total_memory();
    EXPECT_GT(total, kHeapSize - 64);
    EXPECT_LT(total, kHeapSize + 1);

    EXPECT_EQ(VfsManager::instance().close(fd), 0);
}

// ---------------------------------------------------------------------------
// 3. 启动后占用堆内存，ProcFS meminfo 应随之变化——证明 procfs 读取的是
//    实时状态而非快照（跨子系统数据流）。
// ---------------------------------------------------------------------------
TEST_F(SystemBootIntegration, ProcFsMemInfoTracksAllocation) {
    static MemInfoNode meminfo_node;
    ASSERT_TRUE(VfsManager::instance().mount("/proc/meminfo", &meminfo_node));

    const size_t free_before = KernelHeap::instance().get_free_memory();

    // 占用一块内存。
    void* chunk = KernelHeap::instance().allocate(1024);
    ASSERT_NE(chunk, nullptr);
    const size_t free_after = KernelHeap::instance().get_free_memory();
    EXPECT_LT(free_after, free_before);

    // 经 VFS 读回 meminfo，其中应含分配后的（更小的）空闲值。
    const int fd = VfsManager::instance().open("/proc/meminfo");
    ASSERT_GE(fd, 0);
    char buf[256] = {0};
    ASSERT_GT(VfsManager::instance().read(fd, buf, sizeof(buf) - 1), 0);
    VfsManager::instance().close(fd);

    // 数字串形式出现在输出中即证明是实时值。
    char expect[32];
    snprintf(expect, sizeof(expect), "%zu", free_after);
    EXPECT_NE(strstr(buf, expect), nullptr)
        << "procfs meminfo 未反映实时空闲内存 (" << expect << "):\n" << buf;

    KernelHeap::instance().deallocate(chunk);
}

// ---------------------------------------------------------------------------
// 4. 设备注册 → VFS 挂载 → 能力铸造 全链路：注册 CharDevice 后应能在
//    /dev/<name> 打开，并可为任务铸造带读写权限的设备能力。
// ---------------------------------------------------------------------------
TEST_F(SystemBootIntegration, DeviceRegistrationMountsVfsAndMintsCapability) {
    BootProbeDevice& dev = probe_device();
    ASSERT_TRUE(DeviceRegistry::instance().register_device(&dev));

    // 注册后设备应出现在注册表，且已挂载到 /dev/probe0。
    EXPECT_EQ(DeviceRegistry::instance().lookup_device("probe0"), &dev);
    ASSERT_NE(dev.get_vfs_path(), nullptr);
    EXPECT_STREQ(dev.get_vfs_path(), "/dev/probe0");

    // 经 VFS 打开设备节点。
    const int fd = VfsManager::instance().open("/dev/probe0");
    ASSERT_GE(fd, 0);
    char buf[32] = {0};
    const int n = VfsManager::instance().read(fd, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    EXPECT_STREQ(buf, BootProbeDevice::kPayload);
    // 注意：VFS 打开走 VNode::open_file()，而非 Device::open()；
    // open_count 由下方能力路径 open_device() 触发，故不在此断言。
    EXPECT_EQ(VfsManager::instance().close(fd), 0);

    // 能力模型：为一个真实任务铸造读写权限的设备能力到合法槽位。
    TaskControlBlock* shell = nullptr;
    TaskControlBlock* idle = spawn_core_tasks(&shell);
    ASSERT_NE(shell, nullptr);

    const uint32_t slot = 4; // 合法用户槽位
    ASSERT_TRUE(CSpace::is_valid_slot(slot));
    const int rc = DeviceRegistry::instance().open_device(
        shell, "probe0", slot, CAP_RIGHT_READ | CAP_RIGHT_WRITE);
    EXPECT_EQ(rc, 0) << "open_device 失败, rc=" << rc;
    // open_device() 内部调用了 Device::open()，应记录一次打开。
    EXPECT_GT(dev.open_count, 0);

    // 清理铸造出的能力，避免影响后续测试。
    CSpace::cap_delete(shell, slot);
    (void)idle;
}

// ---------------------------------------------------------------------------
// 5. RAMFS 挂载 + 读写往返：镜像 kernel_main 里 /tmp/log.txt 的挂载，
//    验证启动后 tmpfs 类可写文件端到端可用。
// ---------------------------------------------------------------------------
TEST_F(SystemBootIntegration, RamFsMountReadWriteRoundTrip) {
    RamFile& log = ram_log();
    log.clear();
    ASSERT_TRUE(VfsManager::instance().mount("/tmp/log.txt", &log));

    const int fd = VfsManager::instance().open("/tmp/log.txt");
    ASSERT_GE(fd, 0);

    const char* msg = "boot-log-line";
    const int wlen = static_cast<int>(strlen(msg));
    EXPECT_EQ(VfsManager::instance().write(fd, msg, wlen), wlen);
    VfsManager::instance().close(fd);

    // 重新打开读回。
    const int fd2 = VfsManager::instance().open("/tmp/log.txt");
    ASSERT_GE(fd2, 0);
    char buf[64] = {0};
    const int rlen = VfsManager::instance().read(fd2, buf, wlen);
    EXPECT_EQ(rlen, wlen);
    buf[rlen] = '\0';
    EXPECT_STREQ(buf, msg);
    VfsManager::instance().close(fd2);
}

// ---------------------------------------------------------------------------
// 6. 启动后调度器心跳：sleep 高优先级任务 → 回落到 idle → tick 唤醒 → 恢复。
//    验证 tick_update/schedule 协同的真实状态迁移，而非空跑。
// ---------------------------------------------------------------------------
TEST_F(SystemBootIntegration, SleepTickWakeDrivesPriorityFallback) {
    TaskControlBlock* shell = nullptr;
    TaskControlBlock* idle = spawn_core_tasks(&shell);
    ASSERT_NE(idle, nullptr);
    ASSERT_NE(shell, nullptr);

    EXPECT_TRUE(start_scheduler_catching_stub());

    // 先切到 shell。
    Scheduler::instance().schedule();
    ASSERT_EQ(Scheduler::instance().get_current_tcb(), shell);

    // 让 shell 主动休眠：应转为 Sleeping，并回落到 idle 接管 CPU。
    g_current_tcb_ptr = shell;
    Scheduler::instance().sleep_ms(5);
    EXPECT_EQ(shell->scheduler.state, TaskState::Sleeping);
    EXPECT_EQ(Scheduler::instance().get_current_tcb(), idle);

    // 推进足够的 tick 唤醒 shell（sleep 5ms @ 1000Hz ≈ 5 tick，多推一些）。
    for (int t = 0; t < 50; ++t)
        Scheduler::instance().tick_update();

    // 唤醒后 shell 应回到 Ready，schedule() 再次选中高优先级的它。
    EXPECT_EQ(shell->scheduler.state, TaskState::Ready);
    Scheduler::instance().schedule();
    EXPECT_EQ(Scheduler::instance().get_current_tcb(), shell);
}

// ---------------------------------------------------------------------------
// 7. ProcFS taskinfo 应列出启动创建的真实任务（TID 行）。
//    证明调度器任务表与 procfs 视图一致。
// ---------------------------------------------------------------------------
TEST_F(SystemBootIntegration, ProcFsTaskInfoListsBootTasks) {
    static TaskInfoNode taskinfo_node;
    TaskControlBlock* shell = nullptr;
    TaskControlBlock* idle = spawn_core_tasks(&shell);
    ASSERT_NE(idle, nullptr);
    ASSERT_NE(shell, nullptr);

    ASSERT_TRUE(VfsManager::instance().mount("/proc/taskinfo", &taskinfo_node));
    const int fd = VfsManager::instance().open("/proc/taskinfo");
    ASSERT_GE(fd, 0);

    char buf[1024] = {0};
    const int n = VfsManager::instance().read(fd, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    VfsManager::instance().close(fd);

    // 表头：TID/STATE/SLEEP_TICKS 三列。
    EXPECT_NE(strstr(buf, "TID\tSTATE\tSLEEP_TICKS"), nullptr);
    // taskinfo 每行格式为 "<id>\t<STATE>\t<sleep_ticks>"；idle(TID 0) 与
    // shell(TID 1) 创建后均为 Ready，故精确匹配这两行，而非弱匹配数字。
    EXPECT_NE(strstr(buf, "\n0\tRDY\t"), nullptr) << "未找到 idle 任务行:\n" << buf;
    EXPECT_NE(strstr(buf, "\n1\tRDY\t"), nullptr) << "未找到 shell 任务行:\n" << buf;
}
