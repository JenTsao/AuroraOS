/* 
 * auroraOS MiBand 8 (Ambiq Apollo3 Blue) 链接脚本
 * 芯片架构: Cortex-M4F
 * 内存分布: 1MB Flash, 384KB SRAM
 */

/* 定义内核启动时的真实入口点 (汇编文件中的中断向量表重置函数) */
ENTRY(Reset_Handler)

/* ======================================================== */
/* 物理内存分布映射                                          */
/* ======================================================== */
MEMORY
{
    /* 
     * Flash 总计 1MB (1024KB). 
     * Bootloader 占用前 448KB (0x00000000 - 0x0006FFFF).
     * 我们的 App 镜像从偏移 448KB (0x00070000) 处开始，剩余可用 576KB.
     */
    FLASH (rx)  : ORIGIN = 0x00070000, LENGTH = 576K

    /* 
     * SRAM 总计 384KB (0x10000000 开始).
     */
    SRAM  (rwx) : ORIGIN = 0x10000000, LENGTH = 384K
}

/* 定义系统主栈堆的初始顶端 (栈向下生长，设为 SRAM 末尾) */
_estack = ORIGIN(SRAM) + LENGTH(SRAM);

/* 定义内核动态分配堆的大小 (根据之前的内存优化，这里预留 64KB) */
_Min_Heap_Size = 0x10000; 
/* 定义主中断栈的大小 (2KB) */
_Min_Stack_Size = 0x800;

/* Flash region bounds for the kernel's SYS_PRINT pointer validator
   (boot/interrupts.cpp). Must cover the app image's flash range so that
   string literals in .rodata are accepted. */
_flash_start = ORIGIN(FLASH);
_flash_end   = ORIGIN(FLASH) + LENGTH(FLASH);

/* ======================================================== */
/* 段 (Sections) 布局映射                                     */
/* ======================================================== */
SECTIONS
{
    /* 1. 中断向量表与可执行代码段 (存放于只读 FLASH) */
    .text :
    {
        . = ALIGN(4);
        /* 强制中断向量表必须放在镜像的最头部 */
        KEEP(*(.isr_vector)) 
        
        *(.text)           /* .text sections (code) */
        *(.text*)          /* .text* sections (code) */
        *(.rodata)         /* .rodata sections (constants, strings, etc.) */
        *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
        
        . = ALIGN(4);
        _etext = .;        /* 标记 text 段结束，也是 data 段的 Flash 起始加载地址 */
    } > FLASH

    /* 2. 初始化的全局/静态数据段 (存放在 SRAM，但初始值从 FLASH 中拷贝) */
    _sidata = LOADADDR(.data);
    
    .data :
    {
        . = ALIGN(4);
        _sdata = .;        /* 记录 SRAM 中 data 段的起始地址 */
        
        *(.data)           /* .data sections */
        *(.data*)          /* .data* sections */
        
        . = ALIGN(4);
        _edata = .;        /* 记录 SRAM 中 data 段的结束地址 */
    } > SRAM AT > FLASH

    /* 3. 未初始化的全局/静态数据段 (存放在 SRAM，Boot 阶段清零) */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;         /* 记录 bss 段起始地址 */
        __bss_start__ = _sbss;
        
        *(.bss)
        *(.bss*)
        *(COMMON)
        
        . = ALIGN(4);
        _ebss = .;         /* 记录 bss 段结束地址 */
        __bss_end__ = _ebss;
    } > SRAM

    /* 4. 堆栈空间与对齐检查 */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        
        /* 记录堆区的物理起始地址，供 memory.cpp 的 KernelHeap::init() 使用 */
        _heap_start = .;
        
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        
        . = ALIGN(8);
    } > SRAM

    /* 丢弃不需要的调试段 */
    /DISCARD/ :
    {
        libc.a ( * )
        libm.a ( * )
        libgcc.a ( * )
    }
}
