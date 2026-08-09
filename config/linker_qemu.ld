MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

_stack_size = 0x2000;

/* Flash region bounds, used by the kernel's SYS_PRINT pointer validator
   (boot/interrupts.cpp) to accept string literals living in .rodata. Without
   these, the weak symbols default to 0 and every user-space print of a string
   constant is rejected as an invalid pointer. */
_flash_start = ORIGIN(FLASH);
_flash_end   = ORIGIN(FLASH) + LENGTH(FLASH);

SECTIONS
{
    .isr_vector : {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text : {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > FLASH

    _sidata = LOADADDR(.data);

    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    .bss : {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    . = ALIGN(4);
    end = .;
    _end = .;
    __end__ = .;
    _heap_start = .; 
    
    /* 栈顶依然在 RAM 的末尾 */
    _estack = ORIGIN(RAM) + LENGTH(RAM);
    /* 【新增】堆的终点等于栈顶减去预留的栈空间大小 */
    _heap_end = _estack - _stack_size;
}
