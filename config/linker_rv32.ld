/* RISC-V QEMU virt machine linker script
 * 
 * The QEMU 'virt' platform with -bios none expects the kernel
 * entry point at 0x80000000 (start of DRAM). There is no FLASH
 * region — all code and data run directly from RAM.
 *
 * This script places the entire image (text, rodata, data, bss)
 * at 0x80000000, matching QEMU's memory map and the reset vector.
 */
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 128M
}

SECTIONS
{
    .init : {
        _stext = .;
        KEEP(*(.init))
    } > RAM

    .text : {
        *(.text .text.*)
    } > RAM

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > RAM

    /* C++ 全局静态对象构造函数表。没有这三段 + 边界符号，boot.S
     * 就无法遍历调用非平凡构造函数的全局对象（例如 g_oled/g_touch/
     * g_nor_flash），它们会以全零 .bss 状态被直接使用，且不会有任何
     * 编译或链接错误提示——只会在运行时表现为空指针解引用/vtable为空。 */
    .preinit_array : {
        . = ALIGN(4);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > RAM

    .init_array : {
        . = ALIGN(4);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > RAM

    .fini_array : {
        . = ALIGN(4);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > RAM

    . = ALIGN(4);
    _etext = .;

    /* _sidata: source address for .data copy in boot.S.
     * In RAM-only layout (QEMU virt), LMA == VMA — data is already
     * placed by the ELF loader, no copy needed. Alias to _sdata makes
     * the copy loop in boot.S a safe no-op (source == dest → skip). */
    _sidata = .;

    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    . = ALIGN(8);
    _heap_start = .;
    
    _estack = ORIGIN(RAM) + LENGTH(RAM);
    _heap_end = _estack - 0x10000; /* 64KB boot stack */

    /* Flash 边界符号，供 SyscallValidator::validate_user_ptr 校验用。
     * QEMU virt 没有物理 FLASH，整个 image 加载到 RAM，所有 .rodata 字符串
     * 常量也在 RAM 地址空间内。使用整段 RAM 作为 flash 区域让 kernel
     * 调用 sys_print 时能顺利通过只读范围检查。 */
    _flash_start = ORIGIN(RAM);
    _flash_end   = ORIGIN(RAM) + LENGTH(RAM);
}