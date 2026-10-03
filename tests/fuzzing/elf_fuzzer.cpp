// =============================================================================
// tests/fuzzing/elf_fuzzer.cpp — ELF 加载器模糊测试 harness（新增靶子）
//
// 靶子：ElfLoader::load_and_exec()（apps/elf_loader.cpp）——解析不可信 ELF
// 二进制。这是本仓库最典型的攻击面：读取 Elf32_Ehdr / Elf32_Phdr / Elf32_Shdr，
// 对 e_phoff、e_shoff、e_phnum、p_offset、p_filesz、p_memsz、p_vaddr 等大量
// 偏移与大小字段做边界校验、程序头循环、段加载与 W^X 策略判定。任何一处
// 越界读/整数溢出/错误边界都会在 ASAN 下暴露。
//
// 机制：把模糊字节写入一个挂载在 VFS 上的 RamFile，再让 load_and_exec 从
// 真实 VFS 路径读取——完整覆盖 loader 的 open/read/lseek/close 调用链，而非
// 孤立喂内存。前 52 字节强制为合法 ELF 魔数，使 fuzzer 能越过早期的 magic
// 检查、深入到段/节解析逻辑（否则所有输入都在第一道 magic 校验就被拒，
// 覆盖率停滞在入口）。
//
// 由 fuzzer_main.cpp 的 LLVMFuzzerTestOneInput 统一 dispatch 调用。
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../apps/elf.hpp"
#include "../../apps/elf_loader.hpp"
#include "../../vfs/vfs.hpp"
#include "../../vfs/ramfs.hpp"

extern void aurora_fuzz_elf(const uint8_t* data, size_t size) {
    if (size < sizeof(Elf32_Ehdr))
        return;

    // 上限，避免超大文件在 RamFile 里无谓扩容拖慢 fuzzer。
    if (size > 8192u)
        size = 8192u;

    static const char* kPath = "/fuzz/app.elf";

    // 每轮重置 VFS 并重新挂载一个干净的 RamFile。
    VfsManager::instance().init();
    static RamFile ramfile(8192);
    ramfile.clear();
    VfsManager::instance().mount(kPath, &ramfile);

    // 组装文件内容：先放模糊字节，再把"入口门禁"字段覆写为合法值，
    // 让输入能越过 ElfLoader 早期的一致性检查（魔数 / e_machine / e_phentsize），
    // 从而深入到程序段表遍历、段内存分配、W^X 策略、p_offset/p_filesz/p_memsz
    // 边界与整数回绕校验、重定位解析等真正有价值的攻击面。
    //
    // 只钉住这些"结构性常量门禁"，其余字段（e_phoff / e_phnum / e_shoff /
    // e_shnum / e_entry / 各 Phdr 字段 / 段数据）全部保持模糊——它们正是靶点。
    static uint8_t blob[8192];
    memcpy(blob, data, size);

    Elf32_Ehdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(&hdr, blob, size < sizeof(hdr) ? size : sizeof(hdr));
    hdr.e_ident[0] = ELFMAG0;
    hdr.e_ident[1] = ELFMAG1;
    hdr.e_ident[2] = ELFMAG2;
    hdr.e_ident[3] = ELFMAG3;
    // e_ident[4] (class) / e_ident[5] (data) 不被 loader 校验，保留模糊值以扩大覆盖。
    hdr.e_machine = EM_ARM;                    // loader 强制要求 EM_ARM
    hdr.e_phentsize = sizeof(Elf32_Phdr);      // 钉住门禁，否则随机值几乎必然 != 32 而在门口被拒
    hdr.e_shentsize = sizeof(Elf32_Shdr);      // 同理，便于进入节区/重定位解析
    memcpy(blob, &hdr, sizeof(hdr));

    // 写入 VFS 文件。
    int fd = VfsManager::instance().open(kPath);
    if (fd < 0)
        return;
    VfsManager::instance().write(fd, reinterpret_cast<const char*>(blob), static_cast<int>(size));
    VfsManager::instance().close(fd);

    // 驱动 ELF 加载器解析不可信二进制。
    ElfLoader::load_and_exec(kPath);
}
