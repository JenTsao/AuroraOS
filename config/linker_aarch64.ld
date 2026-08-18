ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 128M
}

_flash_start = ORIGIN(RAM);
_flash_end   = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    . = 0x40000000;

    .text.boot : {
        *(.text.boot)
    } > RAM

    .vectors : ALIGN(2048) {
        *(.vectors)
    } > RAM

    .text : ALIGN(4096) {
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    } > RAM

    .data : ALIGN(4096) {
        _sdata = .;
        *(.data .data.*)
        _edata = .;
    } > RAM

    .bss (NOLOAD) : ALIGN(4096) {
        _sbss = .;
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        _ebss = .;
        __bss_end = .;
    } > RAM

    .stack (NOLOAD) : ALIGN(4096) {
        __stack_bottom = .;
        . += 64K;
        __stack_top = .;
    } > RAM

    .heap (NOLOAD) : ALIGN(4096) {
        _heap_start = .;
        __heap_start = .;
        . += 16M;
        _heap_end = .;
        __heap_end = .;
    } > RAM

    _end = .;
}
