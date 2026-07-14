#include "../../../boot/interrupts.hpp"
#include "../../../boot/uart.h"
#include "../../../kernel/task.hpp"
#include "../gic/gic.hpp"
#include "../../../syscall/syscall.hpp"

// We forward some calls to the shared logic in boot/interrupts.cpp
extern "C" {
    void SysTick_Handler();
    void SVC_Handler_C(InterruptFrame* frame);
}

extern "C" void irq_handler_c(InterruptFrame* frame) {
    (void)frame;
    uint32_t int_id = GicV2::acknowledge_interrupt();

    // ID 30 is typically the generic timer (Non-secure EL1 physical timer) on ARMv8
    if (int_id == 30) {
        SysTick_Handler();
    } else if (int_id == 33) { // PL011 UART on QEMU virt is SPI 1 (32+1=33)
        // UART handler
    } else {
        // Unhandled interrupt
        uart_puts("[AArch64] Unhandled IRQ\n");
    }

    GicV2::end_of_interrupt(int_id);
}

extern "C" void svc_handler_c(InterruptFrame* frame) {
    uint64_t svc_num = frame->x[8]; // By convention AArch64 passes syscall number in x8
    
    if (svc_num == SYS_PRINT) {
        const char* str = reinterpret_cast<const char*>(frame->x[0]);
        uart_puts(str);
        frame->x[0] = 0; // Return success
    } else if (svc_num == SYS_YIELD) {
        Scheduler::instance().schedule();
        frame->x[0] = 0;
    } else {
        uart_puts("[AArch64] Unhandled SVC\n");
        frame->x[0] = -1;
    }
}

extern "C" void sync_handler_c(InterruptFrame* frame) {
    (void)frame;
    uart_puts("[AArch64] FATAL: Synchronous Exception (Data/Prefetch Abort)\n");
    while(1);
}

extern "C" void error_handler_c(InterruptFrame* frame) {
    (void)frame;
    uart_puts("[AArch64] FATAL: SError (System Error)\n");
    while(1);
}
