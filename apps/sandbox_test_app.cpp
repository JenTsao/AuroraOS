#include "../kernel/task.hpp"
#include "../syscall/syscall.hpp"

namespace auroraos {
namespace apps {

// This function will be spawned in User Mode!
void sandbox_malicious_task() {
    // 1. Legal Action: System call to print
    sys_print("[SandboxTest] I am a User Mode app. Running legally...\r\n");
    
    // 2. Illegal Action: Try to write to a privileged memory address.
    // The SysTick Control and Status Register (SYST_CSR) is at 0xE000E010.
    // In Cortex-M, the System Control Space (0xE000E000 to 0xE000EFFF) is 
    // ONLY accessible in Privileged mode.
    // If the Sandbox is working, the CPU will instantly throw a HardFault/MemManage 
    // fault here and kill this task!
    sys_print("[SandboxTest] Attempting to hack SysTick (0xE000E010)...\r\n");
    
    volatile uint32_t* syst_csr = reinterpret_cast<volatile uint32_t*>(0xE000E010);
    *syst_csr = 0; // BAM! This should fault!
    
    // If we reach here, the sandbox FAILED.
    sys_print("[SandboxTest] SECURITY BREACH! The sandbox failed!\r\n");
    while (true) {}
}

void run_sandbox_test() {
    sys_print("[SandboxTest] Spawning malicious task in User Mode...\r\n");
    
    uint32_t* app_stack = new uint32_t[256];
    
    // Create the task with TaskPrivilege::User
    auroraos::Scheduler::instance().create_task(
        sandbox_malicious_task, 
        app_stack, 
        256 * sizeof(uint32_t), 
        TaskPriority::Normal, 
        0, 
        TaskPrivilege::User
    );
}

} // namespace apps
} // namespace auroraos
