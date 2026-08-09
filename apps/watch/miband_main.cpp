#include <stdint.h>
#include "miband_kernel.hpp"
#include "memory.hpp"
#include "vfs.hpp"
#include "ramfs.hpp"
#include "photon_cache.hpp"
#include "littlefs_vnode.hpp"
#include "mini_program_engine.hpp"

// 引入由手环专属链接脚本 (linker_miband.ld) 导出的物理内存边界符号
extern "C" uint32_t _heap_start;
extern "C" uint32_t end;

// ========================================================
// auroraOS MiBand 8 物理机总入口
// boot/boot.S 的 Reset_Handler → kernel_entry → bl kernel_main
// 因此必须同时提供 kernel_main 符号以匹配 boot.S 预期。
// ========================================================
extern "C" void kernel_main(void) {
    main();
}

extern "C" int main(void) {
    // 1. 初始化核心内存分配器 (Kernel Heap)
    // 接管 384KB SRAM 中除了静态数据和主栈之外的所有剩余空间
    KernelHeap::instance().init(&_heap_start, &end);

    // 2. 填补架构断层：初始化虚拟文件系统 (VFS)
    VfsManager::instance().init();

    // 3. 挂载物理与内存存储介质
    // 挂载 RamFile 到 /tmp，用于存放运行时的高速临时日志与系统状态缓存
    RamFile* ramfile = new RamFile(4096);
    VfsManager::instance().mount("/tmp", ramfile);

    // 挂载 LittleFS 到 /storage，对接 Apollo3 内置的 1MB Flash 的 App 分区
    // 确保用户的表盘数据、运动历史和 Lua 小程序在掉电后不丢失
    static FlashBlockDevice g_nor_flash("miband_flash", 4096, 128);  // 512KB 仿真闪存
    static PhotonCacheLayer g_photon_cache(g_nor_flash);             // 写缓存层
    static LittleFsAdapter  g_lfs(g_photon_cache, 4096, 128);        // LittleFS 文件系统
    if (g_lfs.mount()) {
        // LittleFS 挂载成功：/storage 目录就绪，供表盘数据与运动历史落盘
        (void)0;
    }

    // 4. 唤醒动态小程序引擎 (Lua Engine)
    // 预加载底层 C++ 绑定的原生 API (如控制震动马达、获取心率、屏幕局部重绘)
    MiniProgramEngine engine;
    engine.init();

    // 5. 核心移交：正式拉起手环微内核调度器
    // 内部将创建 UI 渲染线程、传感器/BLE 守护线程以及 Idle 线程
    miband_kernel_main();

    // 调度器一旦启动（触发 PendSV 进行上下文切换），CPU 控制流将被彻底接管
    // 理论上永远不会执行到此处
    while (1) {
        // 安全兜底死循环
    }

    return 0;
}
