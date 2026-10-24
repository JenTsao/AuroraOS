// =============================================================================
// tests/fuzzing/fuzzer_main.cpp — LibFuzzer 统一入口 + harness 分发器
//
// LibFuzzer 规定一个可执行文件只能有一个 LLVMFuzzerTestOneInput。原先只有本
// 文件的 VFS 靶子被编进构建，network/ipc fuzzer 从未链接、从未运行。
// 现在本入口按输入首字节把模糊数据分发给全部 4 个 harness，让每个靶子都真实
// 执行；fuzzer 引擎仍会分别探索各分支的输入空间。
//
// harness 列表：
//   - aurora_fuzz_vfs   （VfsManager 路径解析，本文件内联）
//   - aurora_fuzz_net   （network_fuzzer.cpp：防火墙包 + 规则命令）
//   - aurora_fuzz_ipc   （ipc_fuzzer.cpp：IPC Endpoint 状态机）
//   - aurora_fuzz_elf   （elf_fuzzer.cpp：ELF 加载器不可信二进制解析）
//
// 各 harness 内部负责自身的输入分片（消费首字节作为模式选择），因此这里把
// **完整** data/size 透传，不做裁剪。
//
// 单靶子模式（CI 夜间长跑用）：
//   AURORA_FUZZ_HARNESS=0..3 可把本进程锁定到单个 harness。首字节 4 路分发
//   会让每个分片把 3/4 的输入预算花在别的靶子上，4 个分片实际跑的是同一份
//   工作；锁定后每片只喂自己的靶子，覆盖深度才能随分片数线性增长。
//   变量未设置或非法时回退到 4 路分发，本地复现与 PR 冒烟测试行为不变。
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../vfs/vfs.hpp"

// 由各 harness 翻译单元提供的靶子入口（C++ linkage）。
extern void aurora_fuzz_net(const uint8_t* data, size_t size);
extern void aurora_fuzz_ipc(const uint8_t* data, size_t size);
extern void aurora_fuzz_elf(const uint8_t* data, size_t size);

// VFS 路径解析靶子（内联在此，保持与原实现一致的行为）。
static void aurora_fuzz_vfs(const uint8_t* data, size_t size) {
    if (size == 0 || size > 255)
        return;

    char path[256];
    memcpy(path, data, size);
    path[size] = '\0';

    int fd = VfsManager::instance().open(path);
    if (fd >= 0) {
        VfsManager::instance().close(fd);
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0)
        return 0;

    // 只读取一次环境变量：本函数每秒被调用上万次，getenv 的字符串扫描
    // 会直接吃掉可观的模糊测试吞吐。
    static const int forced_harness = []() -> int {
        const char* raw = getenv("AURORA_FUZZ_HARNESS");
        if (raw == nullptr || *raw == '\0')
            return -1;
        char* end = nullptr;
        const long value = strtol(raw, &end, 10);
        if (end == raw || *end != '\0' || value < 0 || value > 3)
            return -1;
        return static_cast<int>(value);
    }();

    // 首字节决定本轮驱动哪个 harness，其余字节由 harness 自行解释。
    // 4 路分发，均衡覆盖各靶子；单靶子模式下由环境变量取代首字节。
    const unsigned selector =
        forced_harness >= 0 ? static_cast<unsigned>(forced_harness) : (data[0] & 0x03u);
    switch (selector) {
    case 0:
        aurora_fuzz_vfs(data + 1, size - 1);
        break;
    case 1:
        aurora_fuzz_net(data + 1, size - 1);
        break;
    case 2:
        aurora_fuzz_ipc(data + 1, size - 1);
        break;
    default:
        aurora_fuzz_elf(data + 1, size - 1);
        break;
    }

    return 0;
}
