#ifndef AURORA_APPS_KERNEL_SELFTEST_HPP
#define AURORA_APPS_KERNEL_SELFTEST_HPP

// =============================================================================
// kernel_selftest — on-target behavioural self-test for the REAL kernel
//
// WHY THIS EXISTS
//   The host unit suite (tests/) is valuable but it runs against
//   tests/stubs/arch_api.hpp, which replaces the entire Arch:: layer with
//   no-ops.  Consequently nothing on the host exercises:
//     - the real context switch (PendSV on ARM / trap+ecall on RISC-V)
//     - the SysTick -> schedule() -> switch path
//     - the SVC/ECALL syscall entry and its return-value path
//     - the stack-canary enforcement inside tick_update()
//     - the scheduler watchdog feed
//   A green build does not prove any of that behaves correctly.
//
//   This file runs the same kernel code that ships, on the real target
//   (QEMU), and reports per-case PASS/FAIL plus a machine-greppable
//   summary line:
//
//       [SELFTEST] <case>: PASS|FAIL
//       [SELFTEST] SUMMARY: <passed>/<total>
//       [SELFTEST] RESULT: PASS            <- CI greps this
//
// Enabled per-board with CONFIG_KERNEL_SELFTEST (see boards/*/*.cmake).
// =============================================================================

// Runs the suite in the context of the calling (shell) task and prints the
// report.  Creates and tears down its own probe tasks; safe to run again.
void run_kernel_selftest();

#endif // AURORA_APPS_KERNEL_SELFTEST_HPP
