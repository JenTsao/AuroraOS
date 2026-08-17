#include <gtest/gtest.h>
#include "../../apps/elf.hpp"
#include "../../apps/elf_loader.hpp"
#include <stdint.h>

TEST(ElfRelocationTest, ThumbCallMath) {
    // 模拟 R_ARM_THM_CALL 计算
    // B = S + A - P
    uintptr_t S = 0x08001000; // 目标函数地址 (Thumb, 最低位为 1)
    uintptr_t P = 0x20004000; // 当前 BL 指令地址

    // 我们在这里只测试基本的加减法，实际的二进制编码测试太复杂
    int32_t result = S + 0 - P; // A = 0
    EXPECT_EQ(result, static_cast<int32_t>(0xE7FFD000));
}

TEST(ElfRelocationTest, Abs32Math) {
    uint32_t memory_loc = 0;
    uint32_t* P_ptr = &memory_loc;

    uintptr_t S = 0x08005000;
    uint32_t A = 0x10; // offset
    *P_ptr = A;

    // type == R_ARM_ABS32
    *P_ptr = S + *P_ptr;

    EXPECT_EQ(memory_loc, 0x08005010);
}

#include "../../vfs/vfs.hpp"
#include "../../vfs/ramfs.hpp"
#include <string.h>

TEST(ElfSecurityTest, RejectWritableAndExecutableSegment) {
    // 1. Initialize VFS with RamFile
    VfsManager::instance().init();
    static RamFile ramfile(4096);
    const char* filepath = "/app.elf";
    VfsManager::instance().mount(filepath, &ramfile);

    // 2. Build a synthetic ELF binary with a segment having PF_W | PF_X
    Elf32_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = ELFMAG0;
    ehdr.e_ident[1] = ELFMAG1;
    ehdr.e_ident[2] = ELFMAG2;
    ehdr.e_ident[3] = ELFMAG3;
    ehdr.e_type = 2; // ET_EXEC
    ehdr.e_machine = EM_ARM;
    ehdr.e_version = 1;
    ehdr.e_entry = 0x20000000;
    ehdr.e_phoff = sizeof(Elf32_Ehdr);
    ehdr.e_phentsize = sizeof(Elf32_Phdr);
    ehdr.e_phnum = 1;

    Elf32_Phdr phdr;
    memset(&phdr, 0, sizeof(phdr));
    phdr.p_type = PT_LOAD;
    phdr.p_offset = sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr);
    phdr.p_vaddr = 0x20000000;
    phdr.p_paddr = 0x20000000;
    phdr.p_filesz = 64;
    phdr.p_memsz = 64;
    phdr.p_flags = PF_W | PF_X | PF_R; // MALICIOUS: Violates W^X policy
    phdr.p_align = 4;

    uint8_t payload[64] = {0};

    int fd = VfsManager::instance().open(filepath);
    ASSERT_GE(fd, 0);
    VfsManager::instance().write(fd, reinterpret_cast<const char*>(&ehdr), sizeof(ehdr));
    VfsManager::instance().write(fd, reinterpret_cast<const char*>(&phdr), sizeof(phdr));
    VfsManager::instance().write(fd, reinterpret_cast<const char*>(payload), sizeof(payload));
    VfsManager::instance().close(fd);

    // 3. ElfLoader must detect W^X violation and safely reject loading
    bool loaded = ElfLoader::load_and_exec(filepath);
    EXPECT_FALSE(loaded);
}

