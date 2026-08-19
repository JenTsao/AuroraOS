# AuroraOS Architecture Documentation

Welcome to the internal architecture documentation for AuroraOS.

For the comprehensive, unified architectural contract and subsystem specification, please refer to:
👉 **[Root ARCHITECTURE.md](../../ARCHITECTURE.md)**

---

## Subsystem Architectural Breakdown

### 1. [Kernel Core & Scheduler](../../ARCHITECTURE.md#21-deterministic-real-time-scheduler--synchronization)
- **Algorithm**: $O(1)$ 5-tier priority bitmap scheduling with circular doubly-linked ready queues.
- **Tickless Idle**: Deep-sleep tick compensation calculated up to the nearest timer/IPC deadline.
- **Priority Inversion Protection**: Full Priority Inheritance Protocol (PIP) with multi-lock transitive chain propagation (`kernel/core/mutex.hpp`). (Priority Ceiling Protocol / PCP in roadmap).
- **SMP Readiness**: Single-core optimized for MCU profiles; multi-core blueprint (`PerCpuContext`, `SpinLock`, `IPI`) designed for Cortex-A / multi-hart targets.

### 2. [IPC Subsystem](../../ARCHITECTURE.md#22-ipc-subsystem-inter-process-communication)
- **Communication Model**: Synchronous/asynchronous Endpoint model (`call`, `receive`, `reply`).
- **seL4-Style Badges**: Authentic, unforgeable caller identification delivered to receivers.
- **Message Labels**: Type-based dispatching with multi-worker selective receive (`label_filter`).
- **Deadlines & Non-blocking**: `IPC_NONBLOCK` (`nb_call`, `nb_receive`) and configurable timeout deadlines (`timeout_ms` / `timeout_ticks`).
- **Lifecycle & Revocation**: Automatic waiter cancellation on capability revocation or endpoint destruction to prevent dangling pointers.

### 3. [Capability Security Model (CSpace)](../../ARCHITECTURE.md#23-capability-security-model-cspace)
- **Object Types**: `Endpoint`, `Thread`, `Memory`, `Device`.
- **Operations**: `cap_derive` (attenuation), `cap_mint` (badging), `cap_revoke` (invalidation), `cap_grant` (delegation).
- **Rights**: `CAP_RIGHT_READ`, `CAP_RIGHT_WRITE`, `CAP_RIGHT_GRANT`.

### 4. [Memory Isolation & Validation](../../ARCHITECTURE.md#24-memory-isolation--address-validation)
- **Hardware Mechanisms**: Cortex-M MPU sandbox, RISC-V PMP sandbox, Cortex-A MMU paging.
- **Syscall Pointer Validation**: `SyscallValidator` checking stack, heap, data/BSS boundaries, MMU page maps, and Flash write protection with weak-symbol fallback.

### 5. [Device-as-an-Object Subsystem](../../ARCHITECTURE.md#25-device-as-an-object-model)
- **Device Registry**: `DeviceRegistry` mapping drivers to CSpace capabilities.
- **Unified Syscalls**: `sys_open_device`, `sys_device_read`, `sys_device_write`, `sys_device_ioctl`.

### 6. [Touch & Gesture Subsystem](../../ARCHITECTURE.md#3-subsystem-maturity-matrix)
- **Hardware Driver**: Real I2C GT316 capacitive touch driver.
- **Gesture Engine**: 7-state deterministic gesture recognition state machine.

---

## Subsystem Maturity Overview

Please consult the [Subsystem Maturity Matrix](../../ARCHITECTURE.md#3-subsystem-maturity-matrix) for the authoritative status of each feature (Stable vs Incubating vs Roadmap).
