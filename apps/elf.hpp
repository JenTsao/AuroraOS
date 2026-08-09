#ifndef ELF_HPP
#define ELF_HPP

#include <stdint.h>

// ELF 魔数标识
#define EI_NIDENT 16
#define ELFMAG0   0x7f
#define ELFMAG1   'E'
#define ELFMAG2   'L'
#define ELFMAG3   'F'

// 架构与目标指令集
#define EM_ARM    40      // ARM 架构
#define PT_LOAD   1       // 可加载的程序段 (Program Header Loadable Segment)

// 1. ELF 32位 文件头
struct Elf32_Ehdr {
    uint8_t  e_ident[EI_NIDENT]; // 魔数与硬件标识
    uint16_t e_type;             // 文件类型 (可执行文件 2)
    uint16_t e_machine;          // 目标架构 (EM_ARM)
    uint32_t e_version;          // 版本
    uint32_t e_entry;            // 【绝对核心】程序执行的入口虚拟地址
    uint32_t e_phoff;            // 程序段表在文件中的偏移量
    uint32_t e_shoff;            // 节区表偏移量
    uint32_t e_flags;            // 处理器特定标志
    uint16_t e_ehsize;           // ELF 头部大小
    uint16_t e_phentsize;        // 程序段头部每个条目的大小
    uint16_t e_phnum;            // 程序段的个数
    uint16_t e_shentsize;        // 节区头部大小
    uint16_t e_shnum;            // 节区个数
    uint16_t e_shstrndx;         // 节区名称字符串表索引
};

// Program header flags
#define PF_X 1
#define PF_W 2
#define PF_R 4

// 2. ELF 32位 程序段头 (Program Header)
struct Elf32_Phdr {
    uint32_t p_type;             // 段类型 (必须寻找 PT_LOAD)
    uint32_t p_offset;           // 该段在 ELF 文件中的位置
    uint32_t p_vaddr;            // 期望映射到内存中的虚拟地址
    uint32_t p_paddr;            // 物理地址
    uint32_t p_filesz;           // 在 ELF 文件中所占的实际字节数 (.text + .data)
    uint32_t p_memsz;            // 在内存中所需占用字节数 (多出来的部分就是未初始化的 .bss)
    uint32_t p_flags;            // 读/写/执行 权限
    uint32_t p_align;            // 内存对齐
};

// 3. ELF 32位 节区头 (Section Header)
#define SHT_PROGBITS  1
#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHT_RELA      4
#define SHT_NOBITS    8
#define SHT_REL       9

struct Elf32_Shdr {
    uint32_t sh_name;            // 节区名称索引
    uint32_t sh_type;            // 节区类型
    uint32_t sh_flags;           // 节区标志
    uint32_t sh_addr;            // 执行时的虚拟地址
    uint32_t sh_offset;          // 在文件中的偏移
    uint32_t sh_size;            // 节区大小
    uint32_t sh_link;            // 链接到的另一个节区索引
    uint32_t sh_info;            // 附加信息
    uint32_t sh_addralign;       // 内存对齐
    uint32_t sh_entsize;         // 包含固定大小条目的节区中每个条目的大小
};

// 4. ELF 32位 符号表项 (Symbol Table Entry)
#define ELF32_ST_BIND(i)   ((i)>>4)
#define ELF32_ST_TYPE(i)   ((i)&0xf)
#define ELF32_ST_INFO(b,t) (((b)<<4)+((t)&0xf))

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STB_WEAK   2

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2
#define STT_SECTION 3
#define STT_FILE    4

struct Elf32_Sym {
    uint32_t st_name;            // 符号名称的字符串表索引
    uint32_t st_value;           // 符号的值（地址）
    uint32_t st_size;            // 符号大小
    uint8_t  st_info;            // 类型和绑定属性
    uint8_t  st_other;           // 保留 (可见性)
    uint16_t st_shndx;           // 定义此符号的节区索引
};

// 5. ELF 32位 重定位表项 (Relocation Entry)
#define ELF32_R_SYM(i)  ((i)>>8)
#define ELF32_R_TYPE(i) ((unsigned char)(i))
#define ELF32_R_INFO(s,t) (((s)<<8)+(unsigned char)(t))

// ARM 架构重定位类型
#define R_ARM_ABS32     2
#define R_ARM_THM_CALL  10

struct Elf32_Rel {
    uint32_t r_offset;           // 发生重定位的位置
    uint32_t r_info;             // 符号表索引和重定位类型
};

struct Elf32_Rela {
    uint32_t r_offset;
    uint32_t r_info;
    int32_t  r_addend;
};

#endif
