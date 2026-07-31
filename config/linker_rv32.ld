ENTRY(_start)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x20000000, LENGTH = 32M
    RAM (rwx)  : ORIGIN = 0x80000000, LENGTH = 128M
}

SECTIONS
{
    .text : {
        _stext = .;
        KEEP(*(.init))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH
    _sidata = LOADADDR(.data);

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
    
    _flash_start = ORIGIN(FLASH);
    _flash_end = _etext;
}
