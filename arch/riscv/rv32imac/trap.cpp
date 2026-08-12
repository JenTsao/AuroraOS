#include "../../../boot/interrupts.hpp"
#include "../../../kernel/task/task.hpp"
#include "../../../kernel/core/arch_api.hpp"

extern "C" {
    extern TaskControlBlock* volatile g_current_tcb_ptr;
    extern TaskControlBlock* volatile g_next_tcb_ptr;

    uint32_t* trap_handler_c(uint32_t* sp) {
        // Read mcause
        uint32_t mcause;
        __asm__ volatile ("csrr %0, mcause" : "=r"(mcause));

        bool is_interrupt = (mcause & 0x80000000) != 0;
        uint32_t cause = mcause & 0x7FFFFFFF;

        // Save current sp to TCB
        if (g_current_tcb_ptr) {
            g_current_tcb_ptr->stack_ptr = sp;
        }

        if (is_interrupt) {
            if (cause == 7) { // Machine Timer Interrupt
                extern void uart_puts(const char*);
                uart_puts("[Trap] Machine Timer Interrupt enter\r\n");
                // Acknowledge Timer and schedule next tick
                uint32_t now = Arch::get_cycle();
                // We use tick rate from Kconfig or 1000 Hz if not defined
                uint32_t interval = (Arch::get_cycles_per_us() * 1000000) / 1000; 
                uint64_t target = static_cast<uint64_t>(now) + interval;
                
                volatile uint32_t* mtimecmp = reinterpret_cast<volatile uint32_t*>(0x2004000); // CLINT_MTIMECMP
                mtimecmp[0] = 0xFFFFFFFF; 
                mtimecmp[1] = static_cast<uint32_t>(target >> 32);
                mtimecmp[0] = static_cast<uint32_t>(target & 0xFFFFFFFF);

                SysTick_Handler();
            } else if (cause == 3) { // Machine Software Interrupt (Context Switch / PendSV equivalent)
                // Acknowledge MSIP
                *reinterpret_cast<volatile uint32_t*>(0x2000000) = 0; // CLINT_MSIP

                // SysTick_Handler or yielding tasks set g_next_tcb_ptr.
                // We just need to update g_current_tcb_ptr.
                if (g_next_tcb_ptr) {
                    g_current_tcb_ptr = g_next_tcb_ptr;
                }
            }
        } else {
            if (cause == 8 || cause == 11) { // ECALL from U-mode (8) or M-mode (11)
                // Move mepc past ecall instruction (4 bytes)
                sp[31] += 4; // mepc is at index 31

                // Map RISC-V arguments a0-a7 (x10-x17) to Syscall Arguments
                InterruptFrame frame;
                frame.arg0 = sp[9];  // x10 (a0)
                frame.arg1 = sp[10]; // x11 (a1)
                frame.arg2 = sp[11]; // x12 (a2)
                frame.arg3 = sp[12]; // x13 (a3)
                frame.pc = sp[31] - 4; // PC of ecall
                frame.svc_num = sp[16]; // a7 (x17) is syscall number

                SVC_Handler_C(&frame);
                
                // If the syscall was a yield or block, it may have requested a context switch.
                // It will set CLINT_MSIP in trigger_context_switch().
                // However, on ARM, returning from SVC allows PendSV to execute immediately.
                // On RISC-V, MSIP is an interrupt, so it will trigger as soon as we mret 
                // if MIE=1. That matches behavior!
            } else {
                // Other faults
                extern void uart_puts(const char*);
                uart_puts("\r\n[Fatal] RISC-V CPU Exception / Fault Detected!\r\n");
                
                // Print cause and mepc (simple hex print since aurora_dbg_print_hex might not be available here directly)
                auto print_hex = [](uint32_t val) {
                    char buf[11];
                    buf[0] = '0'; buf[1] = 'x';
                    for (int i = 7; i >= 0; i--) {
                        uint32_t nibble = (val >> (i * 4)) & 0xF;
                        buf[2 + (7 - i)] = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
                    }
                    buf[10] = '\0';
                    uart_puts(buf);
                };
                
                uart_puts("mcause: "); print_hex(mcause); uart_puts("\r\n");
                uart_puts("mepc:   "); print_hex(sp[31]); uart_puts("\r\n");
                
                while (1) {}
            }
        }

        // Return the (possibly updated) stack pointer
        if (g_current_tcb_ptr) {
            return static_cast<uint32_t*>(g_current_tcb_ptr->stack_ptr);
        }
        return sp;
    }
}
