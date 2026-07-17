/*
 * auroraOS Cortex-M0+ 链接脚本
 * 目标芯片: STM32L031K6 (64KB Flash, 8KB SRAM)
 * 架构: ARMv6-M (Cortex-M0+)
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}

_stack_size = 0x800;

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

    /* 栈顶在 RAM 末尾 */
    _estack = ORIGIN(RAM) + LENGTH(RAM);
    /* 堆终点 = 栈顶 - 预留栈空间 */
    _heap_end = _estack - _stack_size;

    /* Flash 边界符号，供 SyscallValidator 校验用 */
    _flash_start = ORIGIN(FLASH);
    _flash_end   = ORIGIN(FLASH) + LENGTH(FLASH);
}
