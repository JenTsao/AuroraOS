# AuroraOS Architecture

AuroraOS is based on the July Kernel, a security-oriented, capability-based microkernel designed for resource-constrained systems (ARM Cortex-M, RISC-V RV32).

## 1. System Layers

The system is strictly layered to prevent dependency cycles. Dependencies flow strictly downwards:

```
+---------------------------------------------------+
|                   Applications                    |
|       (System Tools, Test Programs, Apps)         |
+---------------------------------------------------+
                          |
                          v
+---------------------------------------------------+
|                     Services                      |
|       (VFS, Network, Device Management)           |
+---------------------------------------------------+
                          |
                          v
+---------------------------------------------------+
|               Runtime & Libraries                 |
|            (C Runtime, Frameworks)                |
+---------------------------------------------------+
                          |
                          v
+---------------------------------------------------+
|                   July Kernel                     |
|  (Scheduler, Memory, IPC, Capabilities, Syscalls) |
+---------------------------------------------------+
                          |
                          v
+---------------------------------------------------+
|         Hardware Abstraction Layer (HAL)          |
|    (Device Drivers, Architecture Abstractions)    |
+---------------------------------------------------+
                          |
                          v
+---------------------------------------------------+
|                     Hardware                      |
|           (ARM, RISC-V, Peripherals)              |
+---------------------------------------------------+
```

## 2. Core Kernel Subsystems (July Kernel)

The kernel is intentionally kept small. Complex logic is pushed to userspace services.

- **Scheduler:** Deterministic and preemptive task scheduling.
- **Memory Management:** MPU-based memory isolation and physical page management.
- **IPC (Inter-Process Communication):** Secure message passing between isolated tasks.
- **Capability System:** Object-oriented capability-based security model controlling access to all resources.
- **Interrupt Management:** Handling hardware interrupts and dispatching them to appropriate handlers or driver tasks.

## 3. Data Flow and Communication

- **Task to Task:** Communication happens strictly via kernel-mediated IPC.
- **Driver to Service:** Hardware interrupts are translated into IPC messages sent to userspace services or specialized driver tasks.
- **Capabilities:** No task can access a kernel object, memory region, or hardware device without explicitly holding a capability for it.

## 4. Module Boundaries

- **Kernel:** `kernel/` contains pure logic. It must NOT contain hardware-specific code.
- **Architecture:** `arch/` contains CPU-specific code (e.g., context switching, MPU configurations).
- **HAL & Drivers:** `hal/` and `drivers/` abstract the hardware. Drivers should not call application logic.
- **Services:** `services/` contains daemons running in userspace, such as the network stack or file system.

## 5. Anti-Patterns (Forbidden Dependencies)

- `Kernel` -> `UI` or `Network`
- `Driver` -> `Application`
- `Stable` -> `Experimental`

*Refer to `AGENTS.md` and `CONTRIBUTING.md` for specific coding guidelines and rules.*
