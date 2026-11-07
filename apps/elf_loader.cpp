#include "elf_loader.hpp"
#include "elf.hpp"
#include "vfs.hpp"
#include "memory.hpp"
#include "task.hpp"
#include "syscall.hpp"
#include "../kernel/core/symbol_export.hpp"
#include "../kernel/mm/page_allocator.hpp"
#include <string.h>
#ifdef ARCH_AARCH64
#include "../arch/arm/cortex-a/mmu/mmu_manager.hpp"
#endif

static auroraos::runtime::AppManifest g_last_manifest{};

const auroraos::runtime::AppManifest& ElfLoader::get_last_manifest() {
    return g_last_manifest;
}

// We use sys_print since ElfLoader runs in the context of the user task that called it (shell_task).
// The original prompt used safe_print, but we have migrated to sys_print for isolation.
// Let's create a small wrapper if needed, or just use sys_print directly.

namespace {

// 阶段 1：ELF 头校验（magic / 架构 / 程序头与节头表边界）。
// 成功时填充 ehdr 与文件大小 fsize；失败已打印原因，返回 false。
// fd 的所有权（关闭）由调用方负责。
bool validate_elf_header(int fd, Elf32_Ehdr& ehdr, uint32_t& fsize) {
    VfsManager::instance().lseek(fd, 0, 0);
    int read_bytes = VfsManager::instance().read(fd, reinterpret_cast<char*>(&ehdr), sizeof(ehdr));

    if (read_bytes != sizeof(ehdr) || ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        sys_print("[ElfLoader] Error: Invalid ELF Magic Number!\r\n");
        return false;
    }

    if (ehdr.e_machine != EM_ARM) {
        sys_print("[ElfLoader] Error: Not an ARM Cortex-M architecture binary!\r\n");
        return false;
    }

    if (ehdr.e_phentsize != sizeof(Elf32_Phdr)) {
        sys_print("[ElfLoader] Error: Invalid Program Header size!\r\n");
        return false;
    }

    if (ehdr.e_phnum > 16) {
        sys_print("[ElfLoader] Error: Too many program headers!\r\n");
        return false;
    }

    int file_size = VfsManager::instance().lseek(fd, 0, 2);
    VfsManager::instance().lseek(fd, 0, 0);
    if (file_size < 0) {
        sys_print("[ElfLoader] Error: Cannot determine file size!\r\n");
        return false;
    }
    fsize = static_cast<uint32_t>(file_size);

    // 回绕安全写法：先单独校验偏移，再用减法校验跨度，避免 a+b 无符号回绕绕过检查
    if (ehdr.e_phoff > fsize || static_cast<uint32_t>(ehdr.e_phnum) * ehdr.e_phentsize > fsize - ehdr.e_phoff) {
        sys_print("[ElfLoader] Error: Program Header Table out of bounds!\r\n");
        return false;
    }
    if (ehdr.e_shnum > 0 &&
        (ehdr.e_shoff > fsize || static_cast<uint32_t>(ehdr.e_shnum) * sizeof(Elf32_Shdr) > fsize - ehdr.e_shoff)) {
        sys_print("[ElfLoader] Error: Section Header Table out of bounds!\r\n");
        return false;
    }
    return true;
}

// 阶段 2：第一遍扫描 PT_LOAD 段，做 W^X / 地址回绕 / 数据边界校验，
// 计算内存跨度 [min_vaddr, max_vaddr) 与 total_memsz（含 256KB 硬限制）。
bool scan_load_segments(int fd, const Elf32_Ehdr& ehdr, uint32_t fsize, uint32_t& min_vaddr,
                        uint32_t& max_vaddr, uint32_t& total_memsz) {
    Elf32_Phdr phdr;
    min_vaddr = 0xFFFFFFFF;
    max_vaddr = 0;
    bool has_loadable_segment = false;

    for (int i = 0; i < ehdr.e_phnum; i++) {
        VfsManager::instance().lseek(fd, ehdr.e_phoff + i * ehdr.e_phentsize, 0);
        int phdr_read = VfsManager::instance().read(fd, reinterpret_cast<char*>(&phdr), sizeof(phdr));
        if (phdr_read != sizeof(phdr)) {
            sys_print("[ElfLoader] Error: Cannot read Program Header!\r\n");
            return false;
        }

        if (phdr.p_type == PT_LOAD && phdr.p_memsz > 0) {
            // [安全加固: W^X 策略强制校验]
            // 严禁任何段同时具备 可写(PF_W) 与 可执行(PF_X) 权限，杜绝 Shellcode 写入并执行攻击
            if ((phdr.p_flags & PF_W) && (phdr.p_flags & PF_X)) {
                sys_print(
                    "[ElfLoader] Security Error: Segment violates W^X policy (simultaneous Write + Execute forbidden)!\r\n");
                return false;
            }

            // [安全加固 1] 检查段地址加法回绕溢出
            if (phdr.p_vaddr + phdr.p_memsz < phdr.p_vaddr) {
                sys_print("[ElfLoader] Error: Integer overflow in segment addresses!\r\n");
                return false;
            }

            has_loadable_segment = true;
            if (phdr.p_vaddr < min_vaddr)
                min_vaddr = phdr.p_vaddr;
            if (phdr.p_vaddr + phdr.p_memsz > max_vaddr)
                max_vaddr = phdr.p_vaddr + phdr.p_memsz;

            // 回绕安全写法：p_offset <= fsize 已保证，再用减法校验 p_filesz
            if (phdr.p_offset > fsize || phdr.p_filesz > fsize - phdr.p_offset) {
                sys_print("[ElfLoader] Error: Segment data out of bounds!\r\n");
                return false;
            }

            // 安全检查：防止畸形 ELF 导致缓冲区溢出
            if (phdr.p_filesz > phdr.p_memsz) {
                sys_print("[ElfLoader] Error: p_filesz > p_memsz!\r\n");
                return false;
            }
        }
    }

    if (!has_loadable_segment) {
        sys_print("[ElfLoader] Error: No loadable segments found!\r\n");
        return false;
    }

    // 分配整块连续内存容纳所有段
    total_memsz = max_vaddr - min_vaddr;

    // [安全加固 2] 限制单个 ELF 最大占用内存 (例如硬限制为 256KB)
    if (total_memsz > 256 * 1024) {
        sys_print("[ElfLoader] Error: total_memsz exceeds 256KB limit!\r\n");
        return false;
    }
    return true;
}

// 阶段 3：第二遍将段数据读入整块连续内存，含 TOCTOU 复检与 BSS 清零。
bool load_segment_data(int fd, const Elf32_Ehdr& ehdr, uint32_t fsize, char* segment_memory,
                       uint32_t min_vaddr, uint32_t total_memsz) {
    Elf32_Phdr phdr;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        VfsManager::instance().lseek(fd, ehdr.e_phoff + i * ehdr.e_phentsize, 0);
        int phdr_read = VfsManager::instance().read(fd, reinterpret_cast<char*>(&phdr), sizeof(phdr));
        if (phdr_read != sizeof(phdr)) {
            sys_print("[ElfLoader] Error: Cannot read Program Header (pass 2)!\r\n");
            return false;
        }

        if (phdr.p_type == PT_LOAD && phdr.p_memsz > 0) {
            // [安全加固: W^X 策略防 TOCTOU]
            if ((phdr.p_flags & PF_W) && (phdr.p_flags & PF_X)) {
                sys_print("[ElfLoader] Security Error: Segment violates W^X policy (TOCTOU attempt)!\r\n");
                return false;
            }
            uint32_t offset_in_mem = phdr.p_vaddr - min_vaddr;

            // [安全加固 3] 防止恶意的重叠段/假偏移导致越界写
            if (offset_in_mem > total_memsz || phdr.p_memsz > total_memsz - offset_in_mem) {
                sys_print("[ElfLoader] Error: segment offset out of bounds!\r\n");
                return false;
            }

            // 回绕安全写法：p_offset <= fsize 已保证，再用减法校验 p_filesz (TOCTOU 复检)
            if (phdr.p_offset > fsize || phdr.p_filesz > fsize - phdr.p_offset) {
                sys_print("[ElfLoader] Error: Segment data out of bounds (TOCTOU)!\r\n");
                return false;
            }

            // [安全加固] 重新校验 p_filesz 防止 TOCTOU
            if (phdr.p_filesz > phdr.p_memsz) {
                sys_print("[ElfLoader] Error: p_filesz > p_memsz (TOCTOU attempt)!\r\n");
                return false;
            }

            VfsManager::instance().lseek(fd, phdr.p_offset, 0);
            int actual_read = VfsManager::instance().read(fd, segment_memory + offset_in_mem, phdr.p_filesz);
            if (actual_read < 0)
                actual_read = 0;

            // [安全加固 4] 清零从实际读取到的位置到 p_memsz 的剩余区域，防止未初始化内存泄露
            for (uint32_t b = actual_read; b < phdr.p_memsz; b++) {
                segment_memory[offset_in_mem + b] = 0;
            }
        }
    }
    return true;
}

// 阶段 4：入口地址换算（e_entry 必须落在已加载段跨度内）。
bool resolve_entry_point(const Elf32_Ehdr& ehdr, const char* segment_memory, uint32_t min_vaddr,
                         uint32_t max_vaddr, uintptr_t& raw_entry) {
    if (ehdr.e_entry >= min_vaddr && ehdr.e_entry < max_vaddr) {
        raw_entry = reinterpret_cast<uintptr_t>(segment_memory) + (ehdr.e_entry - min_vaddr);
        return true;
    }
    sys_print("[ElfLoader] Error: e_entry out of bounds!\r\n");
    return false;
}

// 阶段 5a：解析 .aurora_manifest 节。
// sh_name 只保证偏移落在 shstrtab 之内，节名未必有 NUL 终止符，strcmp 会读出
// 堆分配之外（ASan heap-buffer-overflow）。把字面量连同 '\0' 一起做定长
// memcmp，读操作严格有界，同时等价于“存在终止符且精确匹配”。
void parse_embedded_manifest(int fd, const Elf32_Ehdr& ehdr, const Elf32_Shdr* shdrs,
                             auroraos::runtime::AppManifest& embedded_manifest, bool& has_manifest) {
    if (ehdr.e_shstrndx >= ehdr.e_shnum || shdrs[ehdr.e_shstrndx].sh_size == 0 ||
        shdrs[ehdr.e_shstrndx].sh_size > 64 * 1024) {
        return;
    }
    uint32_t shstr_sz = shdrs[ehdr.e_shstrndx].sh_size;
    char* shstrtab = new char[shstr_sz]();
    if (!shstrtab) {
        return;
    }
    VfsManager::instance().lseek(fd, shdrs[ehdr.e_shstrndx].sh_offset, 0);
    if (VfsManager::instance().read(fd, shstrtab, shstr_sz) == static_cast<int>(shstr_sz)) {
        for (int i = 0; i < ehdr.e_shnum; i++) {
            if (shdrs[i].sh_name < shstr_sz) {
                constexpr char kManifestSection[] = ".aurora_manifest";
                const uint32_t name_off = shdrs[i].sh_name;
                if (shstr_sz - name_off >= sizeof(kManifestSection) &&
                    memcmp(shstrtab + name_off, kManifestSection, sizeof(kManifestSection)) == 0 &&
                    shdrs[i].sh_size >= sizeof(auroraos::runtime::AppManifest)) {
                    VfsManager::instance().lseek(fd, shdrs[i].sh_offset, 0);
                    int m_read = VfsManager::instance().read(fd, reinterpret_cast<char*>(&embedded_manifest),
                                                            sizeof(embedded_manifest));
                    if (m_read == sizeof(embedded_manifest) && embedded_manifest.is_valid()) {
                        has_manifest = true;
                        g_last_manifest = embedded_manifest;
                        sys_print("[ElfLoader] Embedded AppManifest loaded: ");
                        sys_print(embedded_manifest.name);
                        sys_print("\r\n");
                        break;
                    }
                }
            }
        }
    }
    delete[] shstrtab;
}

// 阶段 5b：加载符号表与配对联的字符串表。
// 任一环节失败都保持输出为空（与原始行为一致：没有符号表就跳过全部重定位）。
void load_symbol_tables(int fd, const Elf32_Ehdr& ehdr, const Elf32_Shdr* shdrs, Elf32_Sym*& symtab,
                        uint32_t& sym_count, char*& strtab, uint32_t& strtab_size) {
    for (int i = 0; i < ehdr.e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            if (shdrs[i].sh_size > 256 * 1024)
                continue;
            if (symtab) {
                delete[] symtab;
                symtab = nullptr;
            }
            if (strtab) {
                delete[] strtab;
                strtab = nullptr;
            }
            // 旧 strtab 已释放，其大小记录同步失效
            strtab_size = 0;

            if (shdrs[i].sh_size % sizeof(Elf32_Sym) != 0)
                continue;
            sym_count = shdrs[i].sh_size / sizeof(Elf32_Sym);
            symtab = new Elf32_Sym[sym_count]();
            if (!symtab) {
                sys_print("[ElfLoader] Out of Memory for symbol table!\r\n");
                continue;
            }
            VfsManager::instance().lseek(fd, shdrs[i].sh_offset, 0);
            int sym_read =
                VfsManager::instance().read(fd, reinterpret_cast<char*>(symtab), sym_count * sizeof(Elf32_Sym));
            if (sym_read != static_cast<int>(sym_count * sizeof(Elf32_Sym))) {
                delete[] symtab;
                symtab = nullptr;
                continue;
            }

            uint32_t strtab_idx = shdrs[i].sh_link;
            if (strtab_idx < ehdr.e_shnum) {
                if (shdrs[strtab_idx].sh_size > 256 * 1024)
                    continue;
                // 多分配 1 字节并保持其为 '\0'：不可信 ELF 的最后一条符号名可以
                // 恰好贴着字符串表末尾且不带终止符，此时 sym_name 指向的串会一路
                // 读到堆块之外（ASan heap-buffer-overflow READ，见夜间 fuzz
                // elf 靶子 2026-11-07 产出的 crash 制品）。补终止符后
                // lookup_kernel_symbol / sys_print 的扫描必然有界。
                const uint32_t strtab_bytes = shdrs[strtab_idx].sh_size;
                strtab = new char[strtab_bytes + 1]();
                if (!strtab) {
                    sys_print("[ElfLoader] Out of Memory for string table!\r\n");
                    continue;
                }
                VfsManager::instance().lseek(fd, shdrs[strtab_idx].sh_offset, 0);
                int str_read = VfsManager::instance().read(fd, strtab, strtab_bytes);
                if (str_read != static_cast<int>(strtab_bytes)) {
                    delete[] strtab;
                    strtab = nullptr;
                    continue;
                }
                strtab[strtab_bytes] = '\0';
                // 记录与当前 symtab 配对加载的 strtab 大小，供重定位越界校验使用
                strtab_size = strtab_bytes;
            }
        }
    }
}

// 在内核导出符号表中线性查找（UNDEF 符号解析），未找到返回 0。
uintptr_t lookup_kernel_symbol(const char* sym_name) {
    for (int k = 0; k < kernel_symtab_size; k++) {
        bool match = true;
        const char* s1 = sym_name;
        const char* s2 = kernel_symtab[k].name;
        while (*s1 && *s2) {
            if (*s1 != *s2) {
                match = false;
                break;
            }
            s1++;
            s2++;
        }
        if (match && *s1 == '\0' && *s2 == '\0') {
            return kernel_symtab[k].addr;
        }
    }
    return 0;
}

// 重定位项符号地址解析结果。
enum class SymbolResolve { Ok, Skip, Unresolved };

// 解析重定位项引用的符号地址 S。
// Ok：S 有效；Skip：该重定位项应被跳过（已定义符号边界外）；
// Unresolved：UNDEF 符号无法解析（硬错误，由调用方打印并中止加载）。
SymbolResolve resolve_relocation_symbol(const Elf32_Sym& sym, const char* sym_name, const char* segment_memory,
                                        uint32_t min_vaddr, uint32_t max_vaddr, uintptr_t& S) {
    if (sym.st_shndx == 0) { // UNDEF
        S = lookup_kernel_symbol(sym_name);
        return S != 0 ? SymbolResolve::Ok : SymbolResolve::Unresolved;
    }
    // 边界检查：sym.st_value 必须在加载的段地址范围内，
    // 否则 sym.st_value - min_vaddr 会无符号下溢，
    // 导致 S 指向 segment_memory 之外的地址，重定位写入错误数据。
    if (sym.st_value < min_vaddr || sym.st_value >= max_vaddr) {
        return SymbolResolve::Skip;
    }
    S = reinterpret_cast<uintptr_t>(segment_memory) + (sym.st_value - min_vaddr);
    return SymbolResolve::Ok;
}

// R_ARM_ABS32：*(uint32_t*)P += S
void apply_abs32_relocation(uint8_t* segment_memory, uint32_t p_mem_offset, uintptr_t S) {
    uint32_t* P_ptr = reinterpret_cast<uint32_t*>(segment_memory + p_mem_offset);
    uint32_t A = *P_ptr;
    *P_ptr = S + A;
}

// R_ARM_THM_CALL：Thumb BL（32 位 Thumb2 分支）立即数重写，目标为 S + A - P。
void apply_thm_call_relocation(uint8_t* segment_memory, uint32_t p_mem_offset, uintptr_t S) {
    uint16_t* instr = reinterpret_cast<uint16_t*>(segment_memory + p_mem_offset);
    uint16_t hw1 = instr[0];
    uint16_t hw2 = instr[1];

    uint32_t S_bit = (hw1 >> 10) & 1;
    uint32_t J1 = (hw2 >> 13) & 1;
    uint32_t J2 = (hw2 >> 11) & 1;
    uint32_t I1 = ~(J1 ^ S_bit) & 1;
    uint32_t I2 = ~(J2 ^ S_bit) & 1;

    int32_t A = (S_bit << 24) | (I1 << 23) | (I2 << 22) | ((hw1 & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1);
    if (A & 0x01000000) {
        A |= 0xFE000000;
    }

    uintptr_t P = reinterpret_cast<uintptr_t>(segment_memory) + p_mem_offset;
    int32_t result = S + A - P;

    uint32_t new_S = (result >> 24) & 1;
    uint32_t new_I1 = (result >> 23) & 1;
    uint32_t new_I2 = (result >> 22) & 1;
    uint32_t new_J1 = ~(new_I1 ^ new_S) & 1;
    uint32_t new_J2 = ~(new_I2 ^ new_S) & 1;

    instr[0] = (hw1 & 0xF800) | (new_S << 10) | ((result >> 12) & 0x3FF);
    instr[1] = (hw2 & 0xD000) | (new_J1 << 13) | (new_J2 << 11) | ((result >> 1) & 0x7FF);
}

// 阶段 5：节头解析 + Manifest 提取 + 符号表加载 + 重定位应用。
// 临时分配（shdrs / symtab / strtab / rels）在本函数内全部释放；
// segment_memory 的所有权与 fd 的关闭由调用方负责。
bool apply_relocations(int fd, const Elf32_Ehdr& ehdr, char* segment_memory, uint32_t min_vaddr,
                       uint32_t max_vaddr, uint32_t total_memsz, auroraos::runtime::AppManifest& embedded_manifest,
                       bool& has_manifest) {
    if (ehdr.e_shnum == 0) {
        return true; // 无节头：无 Manifest、无重定位
    }
    if (ehdr.e_shnum > 64) {
        sys_print("[ElfLoader] Error: Too many section headers!\r\n");
        return false;
    }
    sys_print("[ElfLoader] Parsing Section Headers for Relocation...\r\n");
    Elf32_Shdr* shdrs = new Elf32_Shdr[ehdr.e_shnum]();
    if (!shdrs) {
        sys_print("[ElfLoader] Out of Memory for section headers!\r\n");
        return false;
    }
    VfsManager::instance().lseek(fd, ehdr.e_shoff, 0);
    int shdr_read =
        VfsManager::instance().read(fd, reinterpret_cast<char*>(shdrs), ehdr.e_shnum * sizeof(Elf32_Shdr));
    if (shdr_read != static_cast<int>(ehdr.e_shnum * sizeof(Elf32_Shdr))) {
        sys_print("[ElfLoader] Error: Cannot read section headers!\r\n");
        delete[] shdrs;
        return false;
    }

    parse_embedded_manifest(fd, ehdr, shdrs, embedded_manifest, has_manifest);

    Elf32_Sym* symtab = nullptr;
    uint32_t sym_count = 0;
    char* strtab = nullptr;
    uint32_t loaded_strtab_size = 0; // 实际加载的 strtab 大小，用于 st_name 越界校验
    load_symbol_tables(fd, ehdr, shdrs, symtab, sym_count, strtab, loaded_strtab_size);

    // 符号表与字符串表必须成对存在，缺一即无重定位可做（与旧行为一致）。
    // 在这里一次性收口，循环体内的解引用路径就不再是「可能为空」，
    // cppcheck 也不会把循环里的 symtab/strtab 解引用判成空指针解引用。
    if (!symtab || !strtab) {
        delete[] symtab;
        delete[] strtab;
        delete[] shdrs;
        return true;
    }

    for (int i = 0; i < ehdr.e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_REL) {
            if (shdrs[i].sh_size > 256 * 1024)
                continue;
            if (shdrs[i].sh_size % sizeof(Elf32_Rel) != 0)
                continue;
            uint32_t rel_count = shdrs[i].sh_size / sizeof(Elf32_Rel);
            Elf32_Rel* rels = new Elf32_Rel[rel_count]();
            if (!rels) {
                sys_print("[ElfLoader] Out of Memory for relocation table!\r\n");
                continue;
            }
            VfsManager::instance().lseek(fd, shdrs[i].sh_offset, 0);
            int rel_read =
                VfsManager::instance().read(fd, reinterpret_cast<char*>(rels), rel_count * sizeof(Elf32_Rel));
            if (rel_read != static_cast<int>(rel_count * sizeof(Elf32_Rel))) {
                delete[] rels;
                continue;
            }

            for (uint32_t r = 0; r < rel_count; r++) {
                uint32_t sym_idx = ELF32_R_SYM(rels[r].r_info);
                uint8_t type = ELF32_R_TYPE(rels[r].r_info);

                if (sym_idx >= sym_count)
                    continue;

                Elf32_Sym& sym = symtab[sym_idx];

                // st_name 越界校验：使用与当前 symtab 配对加载的 strtab 大小，
                // 防止 sym.st_name 指向 strtab 之外导致越界读
                if (sym.st_name >= loaded_strtab_size)
                    continue;

                const char* sym_name = strtab + sym.st_name;

                uintptr_t S = 0;
                SymbolResolve resolved =
                    resolve_relocation_symbol(sym, sym_name, segment_memory, min_vaddr, max_vaddr, S);
                if (resolved == SymbolResolve::Skip) {
                    continue;
                }
                if (resolved == SymbolResolve::Unresolved) {
                    sys_print("[ElfLoader] Error: Unresolved symbol: ");
                    sys_print(sym_name);
                    sys_print("\r\n");
                    delete[] rels;
                    delete[] strtab;
                    delete[] symtab;
                    delete[] shdrs;
                    return false;
                }

                uint32_t P_vaddr = rels[r].r_offset;
                // [安全加固] 先校验 r_offset 落在已加载段跨度内：
                // 若 r_offset < min_vaddr，P_mem_offset 会无符号下溢；随后
                // P_mem_offset + sizeof(uint32_t) 可回绕为小值绕过下方检查，
                // 造成对 segment_memory 之前内存的越界读写。
                if (P_vaddr < min_vaddr || P_vaddr >= max_vaddr)
                    continue;
                uint32_t P_mem_offset = P_vaddr - min_vaddr;
                if (P_mem_offset + sizeof(uint32_t) > total_memsz)
                    continue;

                if (type == R_ARM_ABS32) {
                    apply_abs32_relocation(reinterpret_cast<uint8_t*>(segment_memory), P_mem_offset, S);
                } else if (type == R_ARM_THM_CALL) {
                    apply_thm_call_relocation(reinterpret_cast<uint8_t*>(segment_memory), P_mem_offset, S);
                }
            }
            delete[] rels;
        }
    }

    delete[] strtab;
    delete[] symtab;
    delete[] shdrs;
    return true;
}

#ifdef ARCH_AARCH64
// 阶段 6（AArch64）：按 4KB 页分配物理页、拷贝段数据并按段权限建映射（W^X）。
bool map_aarch64_segment_pages(auroraos::kernel::mmu::AArch64MmuManager* vasp, int fd, const Elf32_Ehdr& ehdr,
                               char* segment_memory, uint32_t min_vaddr, uint32_t total_memsz) {
    for (uint32_t offset = 0; offset < total_memsz; offset += auroraos::kernel::PageAllocator::PAGE_SIZE) {
        void* phys_page = auroraos::kernel::PageAllocator::instance().alloc_page();
        if (!phys_page) {
            sys_print("[ElfLoader] Error: Out of physical pages!\r\n");
            return false;
        }

        uint32_t copy_sz = (total_memsz - offset < auroraos::kernel::PageAllocator::PAGE_SIZE)
                               ? (total_memsz - offset)
                               : auroraos::kernel::PageAllocator::PAGE_SIZE;
        char* dest = reinterpret_cast<char*>(phys_page);
        char* src = segment_memory + offset;
        for (uint32_t b = 0; b < copy_sz; ++b) {
            dest[b] = src[b];
        }

        // MPU/MMU Permissions based on segment flags (W^X protection)
        uint32_t current_vaddr = min_vaddr + offset;
        auroraos::kernel::MapFlags map_flags = auroraos::kernel::MapFlags::User | auroraos::kernel::MapFlags::Read;

        Elf32_Phdr temp_phdr;
        for (int i = 0; i < ehdr.e_phnum; i++) {
            VfsManager::instance().lseek(fd, ehdr.e_phoff + i * ehdr.e_phentsize, 0);
            VfsManager::instance().read(fd, reinterpret_cast<char*>(&temp_phdr), sizeof(temp_phdr));
            if (temp_phdr.p_type == PT_LOAD && temp_phdr.p_memsz > 0) {
                // If page falls within this segment
                if (current_vaddr >= temp_phdr.p_vaddr && current_vaddr < temp_phdr.p_vaddr + temp_phdr.p_memsz) {
                    if (temp_phdr.p_flags & PF_W)
                        map_flags |= auroraos::kernel::MapFlags::Write;
                    if (temp_phdr.p_flags & PF_X) {
                        // Strict W^X: If page is not writable, grant execute (never grant both write and execute)
                        if (!(map_flags & auroraos::kernel::MapFlags::Write)) {
                            map_flags |= auroraos::kernel::MapFlags::Execute;
                        }
                    }
                    break;
                }
            }
        }

        vasp->map(current_vaddr, reinterpret_cast<uintptr_t>(phys_page), map_flags);
    }
    return true;
}
#endif

} // namespace

bool ElfLoader::load_and_exec(const char* filepath) {
    int fd = VfsManager::instance().open(filepath);
    if (fd < 0) {
        sys_print("[ElfLoader] Error: Cannot open executable file!\r\n");
        return false;
    }

    // 1. 读取并校验 ELF 文件头部
    Elf32_Ehdr ehdr;
    uint32_t fsize = 0;
    if (!validate_elf_header(fd, ehdr, fsize)) {
        VfsManager::instance().close(fd);
        return false;
    }

    sys_print("[ElfLoader] Valid ARM ELF detected. Parsing Segments...\r\n");

    // 2. 第一遍遍历：计算所有 PT_LOAD 段在内存中的跨度
    uint32_t min_vaddr = 0;
    uint32_t max_vaddr = 0;
    uint32_t total_memsz = 0;
    if (!scan_load_segments(fd, ehdr, fsize, min_vaddr, max_vaddr, total_memsz)) {
        VfsManager::instance().close(fd);
        return false;
    }

    // 3. 分配整块连续内存容纳所有段
    char* segment_memory = new char[total_memsz]();
    if (!segment_memory) {
        sys_print("[ElfLoader] Out of Memory while allocating segment!\r\n");
        VfsManager::instance().close(fd);
        return false;
    }

    // 4. 第二遍遍历：实际加载数据
    if (!load_segment_data(fd, ehdr, fsize, segment_memory, min_vaddr, total_memsz)) {
        delete[] segment_memory;
        VfsManager::instance().close(fd);
        return false;
    }

    // 5. 计算我们在内存中的真实入口地址
    uintptr_t raw_entry = 0;
    if (!resolve_entry_point(ehdr, segment_memory, min_vaddr, max_vaddr, raw_entry)) {
        delete[] segment_memory;
        VfsManager::instance().close(fd);
        return false;
    }

    // 6. 第三遍遍历：解析 Section Headers 进行重定位
    bool has_manifest = false;
    auroraos::runtime::AppManifest embedded_manifest{};
    if (!apply_relocations(fd, ehdr, segment_memory, min_vaddr, max_vaddr, total_memsz, embedded_manifest,
                           has_manifest)) {
        delete[] segment_memory;
        VfsManager::instance().close(fd);
        return false;
    }

#ifdef ARCH_AARCH64
    void (*app_entry)(void) = reinterpret_cast<void (*)()>(ehdr.e_entry);
#else
    // 【硬核必杀技】Cortex-M 架构必须运行在 Thumb 状态，地址最低位一定要置 1！
    uintptr_t thumb_entry = raw_entry | 0x01;
    void (*app_entry)(void) = reinterpret_cast<void (*)()>(thumb_entry);
#endif

    sys_print("[ElfLoader] Spawning Dynamic Task from RAM...\r\n");

#ifdef ARCH_AARCH64
    auroraos::kernel::mmu::AArch64MmuManager* vasp = new auroraos::kernel::mmu::AArch64MmuManager();
    if (!vasp || !vasp->map_kernel_regions()) {
        sys_print("[ElfLoader] Error: Failed to map kernel regions in user task VAS!\r\n");
        delete[] segment_memory;
        if (vasp)
            delete vasp;
        VfsManager::instance().close(fd);
        return false;
    }

    // Load and Map text/data segment pages
    if (!map_aarch64_segment_pages(vasp, fd, ehdr, segment_memory, min_vaddr, total_memsz)) {
        delete[] segment_memory;
        delete vasp;
        VfsManager::instance().close(fd);
        return false;
    }

    // Map stack page
    uint32_t* app_stack = reinterpret_cast<uint32_t*>(auroraos::kernel::PageAllocator::instance().alloc_page());
    if (!app_stack) {
        sys_print("[ElfLoader] Error: Out of physical pages for task stack!\r\n");
        delete[] segment_memory;
        delete vasp;
        VfsManager::instance().close(fd);
        return false;
    }
    uint32_t stack_size = auroraos::kernel::PageAllocator::PAGE_SIZE;

    // Map stack 1:1 (virtual == physical) to avoid SP address translation issues in create_task
    vasp->map(reinterpret_cast<uintptr_t>(app_stack), reinterpret_cast<uintptr_t>(app_stack),
              auroraos::kernel::MapFlags::User | auroraos::kernel::MapFlags::Read | auroraos::kernel::MapFlags::Write);

    delete[] segment_memory;
#else
    // Cortex-M Path
    // Use PageAllocator to ensure MPU alignment requirements (size_pow2 = 12 -> 4096 bytes)
    uint32_t* app_stack = reinterpret_cast<uint32_t*>(auroraos::kernel::PageAllocator::instance().alloc_page());
    if (!app_stack) {
        sys_print("[ElfLoader] Error: Out of physical pages for task stack!\r\n");
        delete[] segment_memory;
        VfsManager::instance().close(fd);
        return false;
    }
    uint32_t stack_size = auroraos::kernel::PageAllocator::PAGE_SIZE;
#endif

    // [安全加固] 使用 Manifest 中的配置或默认 4096 字节与 Low 优先级
    TaskPriority task_prio = TaskPriority::Low;
    uint8_t size_pow2 = 12;
    if (has_manifest) {
        if (embedded_manifest.stack_size_pow2 >= 10 && embedded_manifest.stack_size_pow2 <= 14) {
            size_pow2 = static_cast<uint8_t>(embedded_manifest.stack_size_pow2);
        }
        if (embedded_manifest.priority <= static_cast<uint32_t>(TaskPriority::Normal)) {
            task_prio = static_cast<TaskPriority>(embedded_manifest.priority);
        }
    }

    TaskControlBlock* tcb =
        Scheduler::instance().create_task(app_entry, app_stack, stack_size, task_prio, size_pow2, TaskPrivilege::User);
    if (!tcb) {
        sys_print("[ElfLoader] Error: task table full, cannot spawn loaded program!\r\n");
#ifdef ARCH_AARCH64
        auroraos::kernel::PageAllocator::instance().free_page(app_stack);
        delete vasp;
#else
        auroraos::kernel::PageAllocator::instance().free_page(app_stack);
        delete[] segment_memory;
#endif
        VfsManager::instance().close(fd);
        return false;
    }

#ifdef ARCH_AARCH64
    tcb->memory.pgdir_base = vasp->get_pgdir_base();
    tcb->memory.vasp = vasp;
#else
    tcb->memory.pgdir_base = reinterpret_cast<uintptr_t>(segment_memory);
    tcb->memory.vasp = nullptr;
#endif

    VfsManager::instance().close(fd);
    return true;
}

bool ElfLoader::read_manifest(const char* filepath, auroraos::runtime::AppManifest& out_manifest) {
    if (!filepath)
        return false;

    int fd = VfsManager::instance().open(filepath);
    if (fd < 0)
        return false;

    Elf32_Ehdr ehdr;
    VfsManager::instance().lseek(fd, 0, 0);
    int read_bytes = VfsManager::instance().read(fd, reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
    if (read_bytes != sizeof(ehdr) || ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        VfsManager::instance().close(fd);
        return false;
    }

    if (ehdr.e_shnum == 0 || ehdr.e_shnum > 64 || ehdr.e_shstrndx >= ehdr.e_shnum) {
        VfsManager::instance().close(fd);
        return false;
    }

    Elf32_Shdr* shdrs = new Elf32_Shdr[ehdr.e_shnum]();
    if (!shdrs) {
        VfsManager::instance().close(fd);
        return false;
    }

    VfsManager::instance().lseek(fd, ehdr.e_shoff, 0);
    int shdr_read = VfsManager::instance().read(fd, reinterpret_cast<char*>(shdrs), ehdr.e_shnum * sizeof(Elf32_Shdr));
    if (shdr_read != static_cast<int>(ehdr.e_shnum * sizeof(Elf32_Shdr))) {
        delete[] shdrs;
        VfsManager::instance().close(fd);
        return false;
    }

    bool found = false;
    uint32_t shstr_sz = shdrs[ehdr.e_shstrndx].sh_size;
    if (shstr_sz > 0 && shstr_sz <= 64 * 1024) {
        char* shstrtab = new char[shstr_sz]();
        if (shstrtab) {
            VfsManager::instance().lseek(fd, shdrs[ehdr.e_shstrndx].sh_offset, 0);
            if (VfsManager::instance().read(fd, shstrtab, shstr_sz) == static_cast<int>(shstr_sz)) {
                for (int i = 0; i < ehdr.e_shnum; i++) {
                    if (shdrs[i].sh_name < shstr_sz) {
                        // 与 load_and_exec 内同名逻辑一致：定长 memcmp 替代 strcmp，
                        // 防止无 NUL 终止符的节名读越界 shstrtab。
                        constexpr char kManifestSection[] = ".aurora_manifest";
                        const uint32_t name_off = shdrs[i].sh_name;
                        if (shstr_sz - name_off >= sizeof(kManifestSection) &&
                            memcmp(shstrtab + name_off, kManifestSection, sizeof(kManifestSection)) == 0 &&
                            shdrs[i].sh_size >= sizeof(auroraos::runtime::AppManifest)) {
                            VfsManager::instance().lseek(fd, shdrs[i].sh_offset, 0);
                            int m_read = VfsManager::instance().read(fd, reinterpret_cast<char*>(&out_manifest),
                                                                    sizeof(out_manifest));
                            if (m_read == sizeof(out_manifest) && out_manifest.is_valid()) {
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
            delete[] shstrtab;
        }
    }

    delete[] shdrs;
    VfsManager::instance().close(fd);
    return found;
}
