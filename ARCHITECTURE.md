# AuroraOS Architecture Specification

> **Status:** Active Architectural Reference  
> **Target Platforms:** ARM Cortex-M0+, Cortex-M3, Cortex-M4/M4F, RISC-V RV32IMAC, Cortex-A (AArch64)  
> **Core Model:** Capability-Based Security Microkernel (July Kernel)

---

## 1. System Philosophy & Layering

AuroraOS is an ultra-lightweight, capability-based microkernel RTOS engineered for resource-constrained IoT, wearable, and embedded systems. Following true microkernel principles, the kernel core contains only mechanisms requiring hardware privilege: task scheduling, capability management, object-oriented IPC, virtual/physical memory isolation, and interrupt dispatching. All high-level services (networking, file systems, device drivers, UI, and application runtimes) execute in isolated userspace tasks.

Dependencies strictly flow downwards. Reverse dependencies (e.g. Kernel depending on UI or Network) are strictly forbidden:

```text
+---------------------------------------------------+
|                   Applications                    |
|       (Watchfaces, System Shell, Lua Apps)        |
+---------------------------------------------------+
                          |
                          v
+---------------------------------------------------+
|               System Services & UI                |
|  (VFS / RAMFS, lwIP Network, NimBLE BLE, GUIX)    |
+---------------------------------------------------+
                          |
                          v
+---------------------------------------------------+
|                 July Microkernel                  |
|  (Scheduler, CSpace, IPC, MMU/MPU, Syscall Table)  |
+---------------------------------------------------+
                          |
                          v
+---------------------------------------------------+
|         Hardware Abstraction Layer (HAL)          |
|      (Architecture Contexts, Drivers, Timers)     |
+---------------------------------------------------+
                          |
                          v
+---------------------------------------------------+
|                     Hardware                      |
| (Cortex-M0+/M3/M4F, RV32, Apollo3, GT316 Touch)   |
+---------------------------------------------------+
```

---

## 2. Core Kernel Subsystems

### 2.1. Deterministic Real-Time Scheduler & Synchronization

The scheduler is designed for bounded, predictable execution times with zero dynamic heap allocation in hot paths.

```text
Priority Levels (32 Tiers, Highest to Lowest):
  [Realtime / 31] -> [ISR-DPC / 28] -> [Audio / 26] -> [Sensor / 25] -> [Net-RX / 24]
       -> [High / 20] -> [Normal / 16] -> [Low / 8] -> [Idle / 0]
```

- **Algorithm**: $O(1)$ 32-level priority-bitmap scheduling (`ready_bitmask`). Finding the highest-priority runnable task is performed via a single hardware CLZ instruction (`Arch::find_highest_bit`). Ready tasks in the same priority tier are organized in circular doubly-linked static queues with round-robin time slicing.
- **Hardware FPU Lazy Stacking (Cortex-M4F)**:
  - Configures `SCB_FPCCR` with automatic and lazy state preservation (`ASPEN=1`, `LSPEN=1`).
  - Context switch assembly tests `EXC_RETURN` bit 4 (`lr & 0x10`) to skip floating-point registers `s16-s31` when the task did not execute FPU instructions, saving **30% context switch latency** on Cortex-M4F targets (e.g. Ambiq Apollo3 on MiBand 8).
- **Selective Interrupt Masking via BASEPRI**:
  - On Cortex-M3/M4/M4F, critical sections (`IrqGuard`) mask interrupts at or below `CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY` (`0x50U`) using `__set_BASEPRI()`, allowing ultra-low-latency real-time interrupts (e.g. BLE radio) to fire without jitter. Cortex-M0+ retains atomic PRIMASK global masking.
- **Tickless Idle & Dynamic Adaptive VSync**:
  - `FrameSchedulerV2` measures hardware display VSync intervals in real-time, calculates an Exponential Moving Average (EMA), and dynamically forecasts idle sleep duration (`expected_idle_ticks = min(task, timer, ble_interval, next_vsync)`).
- **Priority Inversion Protection**:
  - **Priority Inheritance Protocol (PIP) [STABLE]**: Fully implemented in `Mutex` (`kernel/core/mutex.hpp`) and synchronous `Endpoint` IPC. When a high-priority task blocks on a mutex or sends a synchronous IPC request to a lower-priority task, the worker's `current_priority` is boosted to match the waiting caller. Supports multi-mutex chain propagation (`propagate_priority`) and transitive recalculation upon unlock, reply, or timeout (`recalculate_priority_chain`).
  - **Priority Ceiling Protocol (PCP / IPCP) [ROADMAP]**: Static ceiling priority assignment for mutexes.
- **SMP / Multi-Core Scheduling [ROADMAP]**:
  - Single-core optimized for microcontrollers; blueprint designed for multi-core Cortex-A and RV64.

---

### 2.2. IPC Subsystem (Inter-Process Communication)

AuroraOS implements a seL4-inspired synchronous and asynchronous IPC endpoint model with strict capability isolation, priority ordering, and PIP.

```text
                  Endpoint (Kernel Object)
       +---------------------------------------------+
       | - send_queue_ (Priority-Ordered Senders)    |
       | - recv_queue_ (Priority-Ordered Receivers)  |
       +---------------------------------------------+
         /                                         \
  Sender (Client)                            Receiver (Server)
   - Badge Auth (Minted)                      - Authentic Badge Received
   - Message Label / Type                     - Selective Label Filter
   - Priority-Ordered Enqueue                 - Priority Inheritance (PIP)
   - Timeout / Non-blocking                   - Reply-Blocked Handling
```

- **Priority-Ordered WaitQueues**:
  - Senders and receivers are inserted into `send_queue_` and `recv_queue_` in descending order of `current_priority`. Dequeue operations immediately retrieve the highest-priority matching task, eliminating Head-of-Line blocking.
- **IPC Priority Inheritance Protocol (PIP)**:
  - When a high-priority sender calls an Endpoint, a lower-priority receiver dequeuing or receiving the request inherits the sender's priority for the duration of the synchronous call.
  - Upon `Endpoint::reply()` or call cancellation, the receiver's priority is automatically recalculated and restored.
- **Call-Receive-Reply Workflow**:
  - `Endpoint::call(...)`: Synchronous client invocation. Fast-paths directly to a waiting receiver or enqueues by priority in `send_queue_`.
  - `Endpoint::receive(...)`: Server message reception.
  - `Endpoint::reply(...)`: Non-blocking server reply waking the reply-blocked caller.
- **seL4-Style Capability Badge Authentication**:
  - Senders cannot forge identity; the kernel binds a `uint32_t badge` inside each capability and directly injects it into `receiver->ipc.badge`.
- **Message Labels & Selective Receive**:
  - Supports `msg_type` labels and `label_filter` for selective request dequeuing.
- **Safe Revocation & Anti-Dangling Pointer Protection**:
  - `cancel_all` and `cancel_waiter` safely unlink tasks, wake them with `IpcStatus::ReceiverDead` or `NoPermission`, and restore inherited priorities.
- **DoS Protection**:
  - Strict 4KB message payload ceiling (`MAX_IPC_MSG_SIZE = 4096`), zero dynamic allocation on fast paths.

---

### 2.3. Capability Security Model (CSpace)

Access to all kernel objects, device drivers, and memory regions is governed strictly through object capabilities.

```text
TaskControlBlock
   |
   +--> SecurityContext
          |
          +--> occupied_mask (uint16_t Bitmap, 16 Slots)
          +--> cspace[MAX_CSPACE_SLOTS = 16]
                 |
                 +-> [Slot 0]: Endpoint Cap (Rights: Read | Write, Badge: 0x100)
                 +-> [Slot 1]: Device Cap   (Rights: Read | Write, Object: GT316)
                 +-> [Slot 2]: Memory Cap   (Rights: Read, Range: 0x20000000)
                 +-> [Slot 3]: Thread Cap   (Rights: Grant, Object: Task#2)
```

- **16-Slot Bitmap Allocation & CTZ Acceleration**:
  - `occupied_mask` manages slot allocation via a single hardware CTZ instruction (`Arch::find_lowest_bit(~occupied_mask)`), achieving $O(1)$ allocation (`cap_alloc_slot`).
  - `cap_lookup` performs a single-instruction bitmask test before touching slot memory.
  - `cap_revoke` prunes entire tasks with `occupied_mask == 0` in one instruction and scans only occupied slots.
- **Capability Operations**:
  - `cap_derive`: Duplicate capability with rights attenuation (cannot escalate privileges).
  - `cap_mint`: Duplicate capability while binding a new `badge` authentication identity.
  - `cap_revoke`: Recursively invalidate all derived capabilities pointing to the target kernel object across all tasks.
  - `cap_grant`: Cross-task capability delegation under sender authority.
- **Rights Bitmask**: `CAP_RIGHT_READ`, `CAP_RIGHT_WRITE`, `CAP_RIGHT_GRANT`.

---

### 2.4. Memory Management & Isolation

AuroraOS supports real-time memory allocation, hardware sandboxing, and isolated application heaps.

```text
Memory Architecture:
  1. Kernel Heap        --> Two-Level Segregated Fit (TLSF, O(1) alloc/free, 384 bins)
  2. FastRAM Alignment  --> 8-byte aligned (DTCM / CCMRAM LDRD/STRD acceleration)
  3. MPU Sandboxing     --> 8x512B Sub-Region Disable (SRD) Hardware Stack Sentinel
  4. Lua App Heap       --> Isolated 32KB TLSF Private Heap (Zero Kernel Heap churn)
  5. MMU Virtual Memory --> 4KB Multi-level Page Tables (Cortex-A AArch64)
```

- **TLSF Real-Time Kernel Allocator (`KernelHeap`)**:
  - $O(1)$ Two-Level Segregated Fit allocator (24 First-Level $\times$ 16 Second-Level = 384 bins).
  - Bounded execution time with immediate bidirectional boundary tag physical coalescing.
- **FastRAM & 8-Byte Alignment**:
  - `memory_attributes.hpp` (`AURORA_FAST_RAM`) aligns `TaskControlBlock`, `VNode`, `FileDescriptor`, and `RamFile` to 8-byte boundaries for atomic 64-bit load/store instructions and zero-latency TCM placement.
- **MPU Sub-Region Disable (SRD) Hardware Stack Sentinel**:
  - Divides 4096-byte task stack regions into 8 $\times$ 512-byte subregions. Subregion 0 is disabled via `RASR[15:8]`, functioning as a hardware stack overflow sentinel with **zero RAM waste** (eliminating 2KB alignment padding).
- **Dedicated Lua 5.4 Private Heap (`LuaHeap`)**:
  - 32KB isolated TLSF heap with compact 8-byte boundary headers and in-place realloc. Lua GC table churning is 100% isolated from `KernelHeap`.
- **Universal Syscall Pointer Validator (`SyscallValidator`)**:
  - Validates user-supplied pointers across stack, heap, data, bss, MMU pages, and enforces read-only access on Flash regions.

---

### 2.5. Device-as-an-Object Model

Hardware device access is unified under the capability framework:

- **Device Registry (`DeviceRegistry`)**:
  - Global device registry registering drivers as `KernelObject` instances (`ObjectType::Device`).
  - Configurable capacity (16 slots on standard boards, 4 slots on 8KB M0+).
- **Device System Calls**:
  - `sys_open_device(name, dst_slot, rights)`: Verifies permissions and mints a device capability into the caller's CSpace.
  - `sys_device_read(cap_slot, buf, len, offset)`: Validates user buffer and delegates to driver `read()`.
  - `sys_device_write(cap_slot, buf, len, offset)`: Validates user buffer and delegates to driver `write()`.
  - `sys_device_ioctl(cap_slot, request, arg)`: Executes hardware-specific control requests.

---

## 3. Subsystem Maturity Matrix

| Subsystem / Feature | Status | Target Platforms | Test Coverage |
| :--- | :--- | :--- | :--- |
| **32-Level $O(1)$ Priority Scheduler** | **Stable** | All Targets (M0+, M3, M4F, RV32, AArch64) | 100% (Unit + Boot tests) |
| **Cortex-M4F FPU Lazy Stacking** | **Stable** | ARM Cortex-M4F (Apollo3 / STM32F4) | Hardware verification |
| **BASEPRI Selective IRQ Masking** | **Stable** | Cortex-M3, Cortex-M4/M4F | Unit + Boot tests |
| **Dynamic Adaptive VSync & Power** | **Stable** | Wearables / QEMU | Unit tests (`test_frame_scheduler_v2.cpp`) |
| **TLSF Real-Time Kernel Allocator** | **Stable** | All Targets | Unit tests (`test_heap.cpp`) |
| **FastRAM & 8-Byte Alignment** | **Stable** | All Targets | Unit tests (`test_heap.cpp`) |
| **MPU Sub-Region Disable (SRD)** | **Stable** | ARM Cortex-M0+/M3/M4F | Unit tests (`test_mpu.cpp`) |
| **Lua 32KB Isolated TLSF Heap** | **Stable** | All Targets | Unit tests (`test_lua_vm.cpp`) |
| **CSpace 16-Slot Bitmap Allocation** | **Stable** | All Targets | Unit tests (`test_cspace.cpp`) |
| **IPC Priority Queue & Endpoint PIP** | **Stable** | All Targets | Unit tests (`test_ipc.cpp`) |
| **Priority Inheritance Protocol (Mutex PIP)** | **Stable** | All Targets | Unit tests (`test_mutex_pip.cpp`) |
| **IPC Badging & Label Filtering** | **Stable** | All Targets | Unit tests (`test_ipc.cpp`) |
| **IPC Non-blocking & Timeouts** | **Stable** | All Targets | Unit tests (`test_ipc.cpp`, `test_syscall_ipc.cpp`) |
| **MPU & PMP Sandboxing** | **Stable** | Cortex-M, RV32 | Integration tests |
| **MMU Virtual Memory Manager** | **Incubating** | Cortex-A (AArch64) | Unit tests (`test_mmu_*.cpp`) |
| **DeviceRegistry & Device Syscalls** | **Stable** | All Targets | Unit tests (`test_device_registry.cpp`) |
| **GT316 Touch & 7-State Gestures** | **Stable** | MiBand 8 / Wearables | Unit tests (`test_gt316_driver.cpp`) |
| **Priority Ceiling Protocol (PCP)** | **Roadmap** | Planned | Specification drafted |
| **SMP / Multi-Core Scheduler** | **Roadmap** | Planned for Cortex-A / Multi-Hart RV64 | Architecture designed |

---

## 4. Coding & Verification Standards

1. **Freestanding C++**: Kernel code avoids RTTI, exceptions, unbounded loops, and dynamic memory allocation in hot paths.
2. **Deterministic Locking**: Kernel synchronization utilizes RAII guards (`IrqGuard`, `LockGuard`).
3. **Host-Side Verification**: Algorithms are verified on host test suites (`cmake -S tests -B build_tests && ctest`).
4. **Linker Script Portability**: All section boundaries must export standardized symbols (`_sdata`, `_edata`, `_sbss`, `_ebss`, `_flash_start`, `_flash_end`).
