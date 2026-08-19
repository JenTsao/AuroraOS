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
Priority Levels (Highest to Lowest):
  [Realtime = 4] -> [High = 3] -> [Normal = 2] -> [Low = 1] -> [Idle = 0]
```

- **Algorithm**: $O(1)$ priority-bitmap scheduling (`ready_bitmask`). Finding the highest-priority runnable task is performed via CLZ/bitwise instructions. Ready tasks in the same priority tier are organized in circular doubly-linked static queues with round-robin time slicing.
- **Tickless Idle Support**: `Scheduler::get_expected_idle_ticks()` and `compensate_ticks()` enable deep sleep modes during idle periods, accurately calculating sleep duration up to the nearest timer or IPC timeout deadline.
- **Priority Inversion Protection**:
  - **Priority Inheritance Protocol (PIP) [STABLE]**: Fully implemented in `Mutex` (`kernel/core/mutex.hpp`). When a high-priority task blocks on a mutex held by a lower-priority task, the owner's `current_priority` is boosted to match the waiting task. Supports multi-mutex chain propagation (`propagate_priority`) and transitive recalculation upon unlock or timeout (`recalculate_priority_chain`).
  - **Priority Ceiling Protocol (PCP / IPCP) [ROADMAP]**: Static ceiling priority assignment for mutexes to eliminate runtime inheritance overhead and bounded deadlock.
- **SMP / Multi-Core Scheduling [ROADMAP]**:
  - *Current Implementation*: Single-core optimized for bare-metal microcontroller targets (STM32L0, Apollo3, LM3S6965, RV32).
  - *Multi-Core Design Blueprint*: Introduces `PerCpuContext` (per-core ready queues and `current_tcb`), inter-processor interrupts (IPI) for cross-core preemption, spinlocks (`TicketLock` / `IrqSaveSpinLock`), and task core affinity masks (`cpu_affinity`).

---

### 2.2. IPC Subsystem (Inter-Process Communication)

AuroraOS implements a seL4-inspired synchronous and asynchronous IPC endpoint model with strict capability isolation.

```text
                  Endpoint (Kernel Object)
       +---------------------------------------------+
       | - send_queue_ (Waiting Sender Tasks)        |
       | - recv_queue_ (Waiting Receiver Tasks)       |
       +---------------------------------------------+
         /                                         \
  Sender (Client)                            Receiver (Server)
   - Badge Auth (Minted)                      - Authentic Badge Received
   - Message Label / Type                     - Selective Label Filter
   - Timeout / Non-blocking                   - Reply-Blocked Handling
```

- **Call-Receive-Reply Workflow**:
  - `Endpoint::call(sender, msg, len, reply_buf, max_reply_len, timeout_ticks, badge)`: Synchronous client invocation. Fast-paths directly to a waiting receiver or enqueues in `send_queue_`.
  - `Endpoint::receive(receiver, msg_buf, max_len, timeout_ticks, label_filter)`: Server message reception.
  - `Endpoint::reply(receiver, sender_id, reply_msg, len)`: Non-blocking server reply waking the reply-blocked caller.
- **seL4-Style Capability Badge Authentication**:
  - Server endpoints cannot trust client-asserted IDs. The kernel embeds an unforgeable `uint32_t badge` inside each derived/minted `Capability`.
  - Upon message delivery, the kernel writes `sender->ipc.badge` directly into `receiver->ipc.badge`. The server receives cryptographically authentic caller identification without user payload parsing.
- **Message Labels & Selective Receive**:
  - Senders categorize messages by `msg_type` (Label/Opcode).
  - Multi-worker threads listening on a shared Endpoint can specify a `label_filter` to selectively dequeue matching requests, enabling deterministic thread-pool dispatching.
- **Timeouts & Non-Blocking Primitives**:
  - Fully supports `IPC_NONBLOCK` (`nb_call`, `nb_receive`) returning `WouldBlock` immediately when peer is not ready.
  - Granular millisecond deadlines (`timeout_ms` / `timeout_ticks`) integrated with the scheduler to prevent thread lockup.
- **Safe Revocation & Dangling Pointer Prevention**:
  - When an endpoint capability is deleted/revoked (`CSpace::cap_revoke`) or an `Endpoint` object is destroyed, `cancel_all` and `cancel_waiter` immediately unlink pending tasks, wake them with `IpcStatus::ReceiverDead` or `NoPermission`, and nullify `waiting_endpoint` references.
- **DoS Protection**:
  - Strict 4KB message payload hard ceiling (`MAX_IPC_MSG_SIZE = 4096`).
  - Zero dynamic heap allocation on fast paths.

---

### 2.3. Capability Security Model (CSpace)

Access to all kernel objects, device drivers, and memory regions is governed strictly through object capabilities.

```text
TaskControlBlock
   |
   +--> SecurityContext
          |
          +--> cspace[MAX_CSPACE_SLOTS = 16]
                 |
                 +-> [Slot 0]: Endpoint Cap (Rights: Read | Write, Badge: 0x100)
                 +-> [Slot 1]: Device Cap   (Rights: Read | Write, Object: GT316)
                 +-> [Slot 2]: Memory Cap   (Rights: Read, Range: 0x20000000)
                 +-> [Slot 3]: Thread Cap   (Rights: Grant, Object: Task#2)
```

- **Capability Operations**:
  - `cap_derive`: Duplicate capability with rights attenuation (cannot escalate privileges).
  - `cap_mint`: Duplicate capability while binding a new `badge` authentication identity.
  - `cap_revoke`: Recursively invalidate all derived capabilities pointing to the target kernel object across all tasks.
  - `cap_grant`: Cross-task capability delegation under sender authority.
- **Rights Bitmask**: `CAP_RIGHT_READ`, `CAP_RIGHT_WRITE`, `CAP_RIGHT_GRANT`.

---

### 2.4. Memory Isolation & Address Validation

AuroraOS supports diverse hardware memory protection paradigms across microcontrollers and application processors.

```text
Hardware Isolation Models:
  1. ARM Cortex-M0+/M3/M4F  --> MPU Sandboxing (mpu_switch_sandbox)
  2. RISC-V RV32            --> PMP (Physical Memory Protection)
  3. ARM Cortex-A (AArch64)  --> MMU Paging (VirtualAddressSpace, PageAllocator, MmuManager)
```

- **MPU & PMP Sandboxing**:
  - Per-task stack and data protection regions reconfigured during context switch (`arch_switch_context`).
  - User privilege execution executes unprivileged (`Thread Mode Unprivileged / User Mode`).
- **MMU Virtual Address Space Isolation**:
  - `VirtualAddressSpace` & `MmuManager`: 4KB multi-level page table management, kernel/user space boundary enforcement, and demand page allocation (`PageAllocator`).
- **Universal Syscall Pointer Validator (`SyscallValidator`)**:
  - Validates user-supplied pointers across:
    1. Task private stack space;
    2. Kernel/User heap allocations (`KernelHeap::contains`);
    3. Global static data (`.data`) and `.bss` boundaries;
    4. Active task MMU page mappings (`VirtualAddressSpace::is_valid_range`);
    5. Flash/ROM regions with strict read-only enforcement (writes to Flash are immediately rejected).
  - Uses `__attribute__((weak))` boundary symbols (`_sdata`, `_edata`, `_sbss`, `_ebss`, `_flash_start`, `_flash_end`) to guarantee linker portability across all board targets and host tests.

---

### 2.5. Device-as-an-Object Model

Hardware device access is unified under the capability framework:

- **Device Registry (`DeviceRegistry`)**:
  - Global device registry registering drivers as `KernelObject` instances (`ObjectType::Device`).
  - Drivers register with name, type (Block, Char, Display, Input, Network), and operations table (`DeviceOperations`).
- **Device System Calls**:
  - `sys_open_device(name, dst_slot, rights)`: Verifies permissions and mints a device capability into the caller's CSpace.
  - `sys_device_read(cap_slot, buf, len, offset)`: Validates user buffer and delegates to driver `read()`.
  - `sys_device_write(cap_slot, buf, len, offset)`: Validates user buffer and delegates to driver `write()`.
  - `sys_device_ioctl(cap_slot, request, arg)`: Executes hardware-specific control requests.

---

## 3. Subsystem Maturity Matrix

To adhere to transparent engineering standards, system features are categorized into three stability levels:

| Subsystem / Feature | Status | Target Platforms | Test Coverage |
| :--- | :--- | :--- | :--- |
| **$O(1)$ Priority Scheduler** | **Stable** | All Targets (M0+, M3, M4F, RV32, AArch64) | 100% (Unit + Boot tests) |
| **Priority Inheritance Protocol (PIP)** | **Stable** | All Targets | Unit tests (`test_mutex_pip.cpp`) |
| **Capability System (CSpace)** | **Stable** | All Targets | Unit tests (`test_cspace.cpp`) |
| **IPC Endpoints (Call/Recv/Reply)** | **Stable** | All Targets | Unit tests (`test_ipc.cpp`) |
| **IPC Badging & Label Filtering** | **Stable** | All Targets | Unit tests (`test_ipc.cpp`) |
| **IPC Non-blocking & Timeouts** | **Stable** | All Targets | Unit tests (`test_ipc.cpp`, `test_syscall_ipc.cpp`) |
| **MPU & PMP Sandboxing** | **Stable** | Cortex-M, RV32 | Integration tests |
| **MMU Virtual Memory Manager** | **Incubating** | Cortex-A (AArch64) | Unit tests (`test_mmu_*.cpp`) |
| **DeviceRegistry & Device Syscalls** | **Stable** | All Targets | Unit tests (`test_device_registry.cpp`) |
| **GT316 Touch & 7-State Gestures** | **Stable** | MiBand 8 / Wearables | Unit tests (`test_gt316_driver.cpp`) |
| **Priority Ceiling Protocol (PCP)** | **Roadmap** | Planned | Specification drafted |
| **SMP / Multi-Core Scheduler** | **Roadmap** | Planned for Cortex-A / Multi-Hart RV64 | Architecture designed |
| **Kernel WCET Runtime Profiler** | **Roadmap** | Planned | Specification drafted |

---

## 4. Coding & Verification Standards

1. **Freestanding C++**: Kernel code avoids RTTI, exceptions, unbounded loops, and dynamic memory allocation in hot paths.
2. **Deterministic Locking**: Kernel synchronization utilizes RAII guards (`IrqGuard`, `LockGuard`).
3. **Host-Side Verification**: Algorithms are verified on host test suites (`cmake -S tests -B build_tests && ctest`).
4. **Linker Script Portability**: All section boundaries must export standardized symbols (`_sdata`, `_edata`, `_sbss`, `_ebss`, `_flash_start`, `_flash_end`).
