#include "elf_loader.hpp"
#include "elf.hpp"
#include "vfs.hpp"
#include "memory.hpp"
#include "task.hpp"
#include "syscall.hpp"
#include "../kernel/symbol_export.hpp"
#include "../kernel/page_allocator.hpp"
#ifdef ARCH_AARCH64
#include "../arch/arm/cortex-a/mmu/mmu_manager.hpp"
#endif

// We use sys_print since ElfLoader runs in the context of the user task that called it (shell_task).
// The original prompt used safe_print, but we have migrated to sys_print for isolation.
// Let's create a small wrapper if needed, or just use sys_print directly.

bool ElfLoader::load_and_exec(const char* filepath) {
    int fd = VfsManager::instance().open(filepath);
    if (fd < 0) {
        sys_print("[ElfLoader] Error: Cannot open executable file!\r\n");
        return false;
    }

    // 1. 读取并校验 ELF 文件头部
    Elf32_Ehdr ehdr;
    VfsManager::instance().lseek(fd, 0, 0);
    int read_bytes = VfsManager::instance().read(fd, reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
    
    if (read_bytes != sizeof(ehdr) ||
        ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        sys_print("[ElfLoader] Error: Invalid ELF Magic Number!\r\n");
        VfsManager::instance().close(fd);
        return false;
    }

    if (ehdr.e_machine != EM_ARM) {
        sys_print("[ElfLoader] Error: Not an ARM Cortex-M architecture binary!\r\n");
        VfsManager::instance().close(fd);
        return false;
    }

    if (ehdr.e_phentsize != sizeof(Elf32_Phdr)) {
        sys_print("[ElfLoader] Error: Invalid Program Header size!\r\n");
        VfsManager::instance().close(fd);
        return false;
    }
    
    if (ehdr.e_phnum > 16) {
        sys_print("[ElfLoader] Error: Too many program headers!\r\n");
        VfsManager::instance().close(fd);
        return false;
    }

    sys_print("[ElfLoader] Valid ARM ELF detected. Parsing Segments...\r\n");

    // 第一遍遍历：计算所有 PT_LOAD 段在内存中的跨度
    Elf32_Phdr phdr;
    uint32_t min_vaddr = 0xFFFFFFFF;
    uint32_t max_vaddr = 0;
    bool has_loadable_segment = false;

    for (int i = 0; i < ehdr.e_phnum; i++) {
        VfsManager::instance().lseek(fd, ehdr.e_phoff + i * ehdr.e_phentsize, 0);
        int phdr_read = VfsManager::instance().read(fd, reinterpret_cast<char*>(&phdr), sizeof(phdr));
        if (phdr_read != sizeof(phdr)) {
            sys_print("[ElfLoader] Error: Cannot read Program Header!\r\n");
            VfsManager::instance().close(fd);
            return false;
        }

        if (phdr.p_type == PT_LOAD && phdr.p_memsz > 0) {
            // [安全加固 1] 检查段地址加法回绕溢出
            if (phdr.p_vaddr + phdr.p_memsz < phdr.p_vaddr) {
                sys_print("[ElfLoader] Error: Integer overflow in segment addresses!\r\n");
                VfsManager::instance().close(fd);
                return false;
            }

            has_loadable_segment = true;
            if (phdr.p_vaddr < min_vaddr) min_vaddr = phdr.p_vaddr;
            if (phdr.p_vaddr + phdr.p_memsz > max_vaddr) max_vaddr = phdr.p_vaddr + phdr.p_memsz;
            
            // 安全检查：防止畸形 ELF 导致缓冲区溢出
            if (phdr.p_filesz > phdr.p_memsz) {
                sys_print("[ElfLoader] Error: p_filesz > p_memsz!\r\n");
                VfsManager::instance().close(fd);
                return false;
            }
        }
    }

    if (!has_loadable_segment) {
        sys_print("[ElfLoader] Error: No loadable segments found!\r\n");
        VfsManager::instance().close(fd);
        return false;
    }

    // 分配整块连续内存容纳所有段
    uint32_t total_memsz = max_vaddr - min_vaddr;
    
    // [安全加固 2] 限制单个 ELF 最大占用内存 (例如硬限制为 256KB)
    if (total_memsz > 256 * 1024) {
        sys_print("[ElfLoader] Error: total_memsz exceeds 256KB limit!\r\n");
        VfsManager::instance().close(fd);
        return false;
    }

    char* segment_memory = new char[total_memsz]();
    if (!segment_memory) {
        sys_print("[ElfLoader] Out of Memory while allocating segment!\r\n");
        VfsManager::instance().close(fd);
        return false;
    }

    // 第二遍遍历：实际加载数据
    for (int i = 0; i < ehdr.e_phnum; i++) {
        VfsManager::instance().lseek(fd, ehdr.e_phoff + i * ehdr.e_phentsize, 0);
        int phdr_read = VfsManager::instance().read(fd, reinterpret_cast<char*>(&phdr), sizeof(phdr));
        if (phdr_read != sizeof(phdr)) {
            sys_print("[ElfLoader] Error: Cannot read Program Header (pass 2)!\r\n");
            delete[] segment_memory;
            VfsManager::instance().close(fd);
            return false;
        }

        if (phdr.p_type == PT_LOAD && phdr.p_memsz > 0) {
            uint32_t offset_in_mem = phdr.p_vaddr - min_vaddr;
            
            // [安全加固 3] 防止恶意的重叠段/假偏移导致越界写
            if (offset_in_mem > total_memsz || phdr.p_memsz > total_memsz - offset_in_mem) {
                sys_print("[ElfLoader] Error: segment offset out of bounds!\r\n");
                delete[] segment_memory;
                VfsManager::instance().close(fd);
                return false;
            }

            // [安全加固] 重新校验 p_filesz 防止 TOCTOU
            if (phdr.p_filesz > phdr.p_memsz) {
                sys_print("[ElfLoader] Error: p_filesz > p_memsz (TOCTOU attempt)!\r\n");
                delete[] segment_memory;
                VfsManager::instance().close(fd);
                return false;
            }

            VfsManager::instance().lseek(fd, phdr.p_offset, 0);
            int actual_read = VfsManager::instance().read(fd, segment_memory + offset_in_mem, phdr.p_filesz);
            if (actual_read < 0) actual_read = 0;

            // [安全加固 4] 清零从实际读取到的位置到 p_memsz 的剩余区域，防止未初始化内存泄露
            for (uint32_t b = actual_read; b < phdr.p_memsz; b++) {
                segment_memory[offset_in_mem + b] = 0;
            }
        }
    }

    // 计算我们在内存中的真实入口地址
    uintptr_t raw_entry;
    if (ehdr.e_entry >= min_vaddr && ehdr.e_entry < max_vaddr) {
        raw_entry = reinterpret_cast<uintptr_t>(segment_memory) + (ehdr.e_entry - min_vaddr);
    } else {
        sys_print("[ElfLoader] Error: e_entry out of bounds!\r\n");
        delete[] segment_memory;
        VfsManager::instance().close(fd);
        return false;
    }

    // --- 第三遍遍历：解析 Section Headers 进行重定位 ---
    if (ehdr.e_shnum > 0) {
        if (ehdr.e_shnum > 64) {
            sys_print("[ElfLoader] Error: Too many section headers!\r\n");
            delete[] segment_memory;
            VfsManager::instance().close(fd);
            return false;
        }
        sys_print("[ElfLoader] Parsing Section Headers for Relocation...\r\n");
        Elf32_Shdr* shdrs = new Elf32_Shdr[ehdr.e_shnum]();
        VfsManager::instance().lseek(fd, ehdr.e_shoff, 0);
        int shdr_read = VfsManager::instance().read(fd, reinterpret_cast<char*>(shdrs), ehdr.e_shnum * sizeof(Elf32_Shdr));
        if (shdr_read != static_cast<int>(ehdr.e_shnum * sizeof(Elf32_Shdr))) {
            sys_print("[ElfLoader] Error: Cannot read section headers!\r\n");
            delete[] shdrs;
            delete[] segment_memory;
            VfsManager::instance().close(fd);
            return false;
        }

        Elf32_Sym* symtab = nullptr;
        uint32_t sym_count = 0;
        char* strtab = nullptr;

        for (int i = 0; i < ehdr.e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_SYMTAB) {
                if (shdrs[i].sh_size > 256 * 1024) continue;
                if (symtab) { delete[] symtab; symtab = nullptr; }
                if (strtab) { delete[] strtab; strtab = nullptr; }
                
                if (shdrs[i].sh_size % sizeof(Elf32_Sym) != 0) continue;
                sym_count = shdrs[i].sh_size / sizeof(Elf32_Sym);
                symtab = new Elf32_Sym[sym_count]();
                VfsManager::instance().lseek(fd, shdrs[i].sh_offset, 0);
                int sym_read = VfsManager::instance().read(fd, reinterpret_cast<char*>(symtab), sym_count * sizeof(Elf32_Sym));
                if (sym_read != static_cast<int>(sym_count * sizeof(Elf32_Sym))) {
                    delete[] symtab; symtab = nullptr;
                    continue;
                }
                
                uint32_t strtab_idx = shdrs[i].sh_link;
                if (strtab_idx < ehdr.e_shnum) {
                    if (shdrs[strtab_idx].sh_size > 256 * 1024) continue;
                    strtab = new char[shdrs[strtab_idx].sh_size]();
                    VfsManager::instance().lseek(fd, shdrs[strtab_idx].sh_offset, 0);
                    int str_read = VfsManager::instance().read(fd, strtab, shdrs[strtab_idx].sh_size);
                    if (str_read != static_cast<int>(shdrs[strtab_idx].sh_size)) {
                        delete[] strtab; strtab = nullptr;
                        continue;
                    }
                }
            }
        }

        for (int i = 0; i < ehdr.e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_REL && symtab && strtab) {
                if (shdrs[i].sh_size > 256 * 1024) continue;
                if (shdrs[i].sh_size % sizeof(Elf32_Rel) != 0) continue;
                uint32_t rel_count = shdrs[i].sh_size / sizeof(Elf32_Rel);
                Elf32_Rel* rels = new Elf32_Rel[rel_count]();
                VfsManager::instance().lseek(fd, shdrs[i].sh_offset, 0);
                int rel_read = VfsManager::instance().read(fd, reinterpret_cast<char*>(rels), rel_count * sizeof(Elf32_Rel));
                if (rel_read != static_cast<int>(rel_count * sizeof(Elf32_Rel))) {
                    delete[] rels;
                    continue;
                }

                for (uint32_t r = 0; r < rel_count; r++) {
                    uint32_t sym_idx = ELF32_R_SYM(rels[r].r_info);
                    uint8_t  type    = ELF32_R_TYPE(rels[r].r_info);

                    if (sym_idx >= sym_count) continue;
                    
                    Elf32_Sym& sym = symtab[sym_idx];
                    
                    // st_name bounding check
                    uint32_t strtab_idx = shdrs[i].sh_link; // Need to verify we link to same strtab
                    // Since we stored symtab and strtab globally in our temp vars, we can just use the known strtab size.
                    // But we didn't save strtab_size. Let's find it.
                    uint32_t strtab_size = 0;
                    for (int j = 0; j < ehdr.e_shnum; j++) {
                        if (shdrs[j].sh_type == SHT_SYMTAB && shdrs[j].sh_link < ehdr.e_shnum) {
                            strtab_size = shdrs[shdrs[j].sh_link].sh_size;
                            break;
                        }
                    }
                    if (sym.st_name >= strtab_size) continue;
                    
                    const char* sym_name = strtab + sym.st_name;
                    
                    uintptr_t S = 0;
                    if (sym.st_shndx == 0) { // UNDEF
                        for (int k = 0; k < kernel_symtab_size; k++) {
                            bool match = true;
                            const char* s1 = sym_name;
                            const char* s2 = kernel_symtab[k].name;
                            while (*s1 && *s2) {
                                if (*s1 != *s2) { match = false; break; }
                                s1++; s2++;
                            }
                            if (match && *s1 == '\0' && *s2 == '\0') {
                                S = kernel_symtab[k].addr;
                                break;
                            }
                        }
                        if (S == 0) {
                            sys_print("[ElfLoader] Error: Unresolved symbol: ");
                            sys_print(sym_name);
                            sys_print("\r\n");
                            delete[] rels;
                            if (strtab) delete[] strtab;
                            if (symtab) delete[] symtab;
                            delete[] shdrs;
                            delete[] segment_memory;
                            VfsManager::instance().close(fd);
                            return false;
                        }
                    } else {
                        S = reinterpret_cast<uintptr_t>(segment_memory) + (sym.st_value - min_vaddr);
                    }

                    uint32_t P_vaddr = rels[r].r_offset;
                    uint32_t P_mem_offset = P_vaddr - min_vaddr;
                    if (P_mem_offset + sizeof(uint32_t) > total_memsz) continue;

                    uint32_t* P_ptr = reinterpret_cast<uint32_t*>(segment_memory + P_mem_offset);

                    if (type == R_ARM_ABS32) {
                        uint32_t A = *P_ptr;
                        *P_ptr = S + A;
                    } else if (type == R_ARM_THM_CALL) {
                        uint16_t* instr = reinterpret_cast<uint16_t*>(P_ptr);
                        uint16_t hw1 = instr[0];
                        uint16_t hw2 = instr[1];

                        uint32_t S_bit = (hw1 >> 10) & 1;
                        uint32_t J1 = (hw2 >> 13) & 1;
                        uint32_t J2 = (hw2 >> 11) & 1;
                        uint32_t I1 = ~(J1 ^ S_bit) & 1;
                        uint32_t I2 = ~(J2 ^ S_bit) & 1;

                        int32_t A = (S_bit << 24) | (I1 << 23) | (I2 << 22) |
                                    ((hw1 & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1);
                        if (A & 0x01000000) { A |= 0xFE000000; } 

                        uintptr_t P = reinterpret_cast<uintptr_t>(segment_memory) + P_mem_offset;
                        int32_t result = S + A - P;

                        uint32_t new_S = (result >> 24) & 1;
                        uint32_t new_I1 = (result >> 23) & 1;
                        uint32_t new_I2 = (result >> 22) & 1;
                        uint32_t new_J1 = ~(new_I1 ^ new_S) & 1;
                        uint32_t new_J2 = ~(new_I2 ^ new_S) & 1;

                        instr[0] = (hw1 & 0xF800) | (new_S << 10) | ((result >> 12) & 0x3FF);
                        instr[1] = (hw2 & 0xD000) | (new_J1 << 13) | (new_J2 << 11) | ((result >> 1) & 0x7FF);
                    }
                }
                delete[] rels;
            }
        }

        if (strtab) delete[] strtab;
        if (symtab) delete[] symtab;
        delete[] shdrs;
    }

    
    // 【硬核必杀技】Cortex-M 架构必须运行在 Thumb 状态，地址最低位一定要置 1！
    uintptr_t thumb_entry = raw_entry | 0x01;
    void (*app_entry)(void) = reinterpret_cast<void (*)()>(thumb_entry);

    sys_print("[ElfLoader] Spawning Dynamic Task from RAM...\r\n");

#ifdef ARCH_AARCH64
    auroraos::kernel::mmu::AArch64MmuManager* vasp = new auroraos::kernel::mmu::AArch64MmuManager();
    
    // Load and Map text/data segment pages
    for (uint32_t offset = 0; offset < total_memsz; offset += auroraos::kernel::PageAllocator::PAGE_SIZE) {
        void* phys_page = auroraos::kernel::PageAllocator::instance().alloc_page();
        if (!phys_page) {
            sys_print("[ElfLoader] Error: Out of physical pages!\r\n");
            delete[] segment_memory;
            delete vasp;
            VfsManager::instance().close(fd);
            return false;
        }
        
        uint32_t copy_sz = (total_memsz - offset < auroraos::kernel::PageAllocator::PAGE_SIZE) ? (total_memsz - offset) : auroraos::kernel::PageAllocator::PAGE_SIZE;
        char* dest = reinterpret_cast<char*>(phys_page);
        char* src = segment_memory + offset;
        for(uint32_t b = 0; b < copy_sz; ++b) {
            dest[b] = src[b];
        }
        
        // MPU/MMU Permissions based on segment flags (W^X protection)
        uint32_t current_vaddr = min_vaddr + offset;
        uint32_t map_flags = auroraos::kernel::MapFlags::User | auroraos::kernel::MapFlags::Read;
        
        Elf32_Phdr temp_phdr;
        for (int i = 0; i < ehdr.e_phnum; i++) {
            VfsManager::instance().lseek(fd, ehdr.e_phoff + i * ehdr.e_phentsize, 0);
            VfsManager::instance().read(fd, reinterpret_cast<char*>(&temp_phdr), sizeof(temp_phdr));
            if (temp_phdr.p_type == PT_LOAD && temp_phdr.p_memsz > 0) {
                // If page falls within this segment
                if (current_vaddr >= temp_phdr.p_vaddr && current_vaddr < temp_phdr.p_vaddr + temp_phdr.p_memsz) {
                    if (temp_phdr.p_flags & PF_W) map_flags |= auroraos::kernel::MapFlags::Write;
                    if (temp_phdr.p_flags & PF_X) map_flags |= auroraos::kernel::MapFlags::Execute;
                    break;
                }
            }
        }
        
        vasp->map(current_vaddr, reinterpret_cast<uintptr_t>(phys_page), map_flags);
    }
    
    // Map stack page
    uint32_t* app_stack = reinterpret_cast<uint32_t*>(auroraos::kernel::PageAllocator::instance().alloc_page());
    uint32_t stack_size = auroraos::kernel::PageAllocator::PAGE_SIZE;
    
    // Map stack 1:1 (virtual == physical) to avoid SP address translation issues in create_task
    vasp->map(reinterpret_cast<uintptr_t>(app_stack), reinterpret_cast<uintptr_t>(app_stack), auroraos::kernel::MapFlags::User | auroraos::kernel::MapFlags::Read | auroraos::kernel::MapFlags::Write);
    
    delete[] segment_memory;
#else
    // Cortex-M Path
    // Use PageAllocator to ensure MPU alignment requirements (size_pow2 = 12 -> 4096 bytes)
    uint32_t* app_stack = reinterpret_cast<uint32_t*>(auroraos::kernel::PageAllocator::instance().alloc_page());
    uint32_t stack_size = auroraos::kernel::PageAllocator::PAGE_SIZE;
#endif

    // [安全加固] size_pow2 传 12 (对应 4096 字节), 修复传 0 导致 MPU 沙盒配置失败的问题
    TaskControlBlock* tcb = Scheduler::instance().create_task(app_entry, app_stack, stack_size, TaskPriority::Low, 12, TaskPrivilege::User);
    if (!tcb) {
        sys_print("[ElfLoader] Error: task table full, cannot spawn loaded program!\r\n");
#ifdef ARCH_AARCH64
        auroraos::kernel::PageAllocator::instance().free_page(app_stack);
        delete vasp;
#else
        delete[] app_stack;
        delete[] segment_memory;
#endif
        VfsManager::instance().close(fd);
        return false;
    }

#ifdef ARCH_AARCH64
    tcb->pgdir_base = vasp->get_pgdir_base();
    tcb->vasp_ptr = vasp;
#endif

    VfsManager::instance().close(fd);
    return true;
}
