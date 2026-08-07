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
}
