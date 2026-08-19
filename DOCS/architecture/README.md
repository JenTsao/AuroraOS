# AuroraOS Architecture Documentation

Welcome to the internal architecture documentation for AuroraOS.

For the comprehensive, unified architectural contract and subsystem specification, please refer to:
👉 **[Root ARCHITECTURE.md](../../ARCHITECTURE.md)**

---

## Subsystem Architectural Breakdown

### 1. [Kernel Core & Scheduler](../../ARCHITECTURE.md#21-deterministic-real-time-scheduler--synchronization)
- **Algorithm**: $O(1)$ 32-tier priority bitmap scheduling with hardware single-instruction CLZ bit extraction. Includes sub-tiers for ISR-DPC, Audio, Sensor, and Net-RX.
- **Hardware FPU Lazy Stacking**: Cortex-M4F `SCB_FPCCR` ASPEN/LSPEN hardware lazy stacking saving 30% context switch latency.
- **BASEPRI Selective Masking**: Masking syscall priority interrupts on Cortex-M3/M4/M4F while preserving zero-jitter execution for real-time interrupts (BLE).
- **Tickless Idle & Dynamic Adaptive VSync**: `FrameSchedulerV2` hardware VSync interval measurement, EMA smoothing, and dynamic expected idle ticks prediction.
- **Priority Inversion Protection**: Full Priority Inheritance Protocol (PIP) with multi-lock transitive chain propagation (`kernel/core/mutex.hpp`) and synchronous IPC endpoint inheritance.

### 2. [IPC Subsystem](../../ARCHITECTURE.md#22-ipc-subsystem-inter-process-communication)
- **Communication Model**: Synchronous/asynchronous Endpoint model (`call`, `receive`, `reply`).
- **Priority Ordering & PIP**: Descending priority-ordered `send_queue_` and `recv_queue_` to eliminate Head-of-Line blocking, paired with synchronous IPC priority inheritance and automatic restoration upon reply/cancellation.
- **seL4-Style Badges**: Authentic, unforgeable caller identification delivered to receivers.
- **Message Labels**: Type-based dispatching with multi-worker selective receive (`label_filter`).
- **Deadlines & Non-blocking**: `IPC_NONBLOCK` (`nb_call`, `nb_receive`) and configurable timeout deadlines (`timeout_ms` / `timeout_ticks`).
- **Lifecycle & Revocation**: Automatic waiter cancellation on capability revocation or endpoint destruction to prevent dangling pointers.

### 3. [Capability Security Model (CSpace)](../../ARCHITECTURE.md#23-capability-security-model-cspace)
- **Hardware Bitmap Management**: `uint16_t occupied_mask` with $O(1)$ CTZ allocation (`cap_alloc_slot`), single-instruction bitmask lookup, and bitmap-pruned global capability revocation.
- **Object Types**: `Endpoint`, `Thread`, `Memory`, `Device`.
- **Operations**: `cap_derive` (attenuation), `cap_mint` (badging), `cap_revoke` (invalidation), `cap_grant` (delegation).
- **Rights**: `CAP_RIGHT_READ`, `CAP_RIGHT_WRITE`, `CAP_RIGHT_GRANT`.

### 4. [Memory Management & Isolation](../../ARCHITECTURE.md#24-memory-management--isolation)
- **TLSF Real-Time Allocator**: $O(1)$ Two-Level Segregated Fit allocator (384 bins) with immediate bidirectional boundary tag coalescing.
- **FastRAM & 8-Byte Alignment**: `memory_attributes.hpp` (`AURORA_FAST_RAM`) aligning TCB, VNode, FileDescriptor, and RamFile to 8 bytes for Cortex-M LDRD/STRD and DTCM/CCMRAM.
- **MPU Sub-Region Disable (SRD)**: 8×512B subregions per 4KB page with subregion 0 disabled as a zero-RAM-overhead hardware stack overflow guard.
- **Dedicated Lua 5.4 Private Heap**: 32KB isolated `LuaHeap` with 8-byte boundary tags isolating Lua GC and table churning from `KernelHeap`.
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
