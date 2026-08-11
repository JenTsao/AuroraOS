# AGENTS.md — AuroraOS AI Coding Agent Guide

> **Purpose:** This document defines how AI coding agents must understand, modify, test, and review AuroraOS.
>
> AuroraOS is a security-oriented, capability-based microkernel RTOS for resource-constrained ARM Cortex-M and RISC-V RV32 systems. It is primarily written in freestanding C++ with C device drivers and architecture-specific assembly.
>
> **This document is an engineering contract, not merely documentation.**
>
> AI agents MUST follow these rules unless the user explicitly overrides them.

---

# 1. Project Identity

AuroraOS is a from-scratch microkernel operating system targeting:

- ARM Cortex-M0+
- ARM Cortex-M3
- ARM Cortex-M4/M4F
- RISC-V RV32
- QEMU simulation targets
- wearable and IoT-class hardware

Core architectural goals:

- capability-based resource access
- isolated userspace
- MPU-based memory isolation where supported
- object-oriented IPC
- deterministic/preemptive scheduling
- small kernel core
- hardware abstraction
- security-first system services
- host-side verification of kernel algorithms

AuroraOS is a research/development operating system.

Do not assume that every documented feature is production-ready.

When documentation and implementation disagree:

1. Prefer the actual implementation.
2. Verify tests and build configuration.
3. Do not silently claim an experimental feature is stable.
4. Update documentation when appropriate.

---

# 2. Architectural Principles

AuroraOS follows these principles.

## 2.1 Keep the kernel small

The kernel should contain only functionality that requires kernel privilege or hardware access.

Prefer:

```text
Kernel
├── Scheduler
├── IPC
├── Capability
├── Memory
├── Interrupts
├── Syscalls
└── Architecture abstraction
```

over placing high-level services directly inside the kernel.

Networking, UI, scanners, application logic, Lua services, and other complex functionality should remain outside the kernel unless there is a documented architectural reason.

---

# 3. Repository Layout

Before modifying code, inspect the relevant subsystem.

```text
kernel/             Microkernel core
arch/               Architecture-specific code
boards/             Board definitions and board-specific configuration
drivers/            Hardware drivers
adapter/            HAL/adapter layers
syscall/            System-call definitions and ABI
boot/               Early boot code
bootloader/         Bootloader
vfs/                Virtual filesystem
net/                Networking and network services
apps/               Userspace applications
ui/                 UI framework
ai/                 AI/ML integration
metrics/            Runtime metrics
tests/              Host-side tests
experimental/       Experimental/research implementations
scripts/            Build/configuration helpers
config/             Linker/Kconfig configuration
3rdparty/           Vendored external dependencies
docs/               Architecture and project documentation
.github/            CI configuration
```

---

# 4. Stable vs Experimental Code

`experimental/` is not part of the stable architecture.

AI agents MUST NOT introduce stable-kernel dependencies on experimental code.

Allowed:

```text
experimental
      ↓
stable APIs
```

Forbidden:

```text
stable kernel
      ↓
experimental implementation
```

Do not move experimental code into stable directories merely to make compilation easier.

A feature should graduate from experimental only when it has:

- documented ownership
- defined API
- tests
- error handling
- resource limits
- architecture considerations
- security review
- stable dependency direction

---

# 5. Architecture Boundaries

The dependency direction should generally be:

```text
Applications
     ↓
Services
     ↓
Subsystem APIs
     ↓
Kernel / Syscalls
     ↓
HAL / Architecture
     ↓
Hardware
```

Avoid reverse dependencies.

Examples:

```text
Kernel → UI             forbidden
Kernel → application    forbidden
Kernel → scanner        forbidden
Kernel → experimental   forbidden
Driver → application    generally forbidden
```

The kernel may expose interfaces used by those layers, but must not depend on their implementations.

---

# 6. Kernel Core Rules

The following components are considered high-risk:

- scheduler
- task/TCB
- IPC
- capability system
- memory manager
- syscall dispatcher
- interrupt handling
- architecture context switching
- MPU/MMU management

Changes to these components require additional review.

Do not modify kernel interfaces merely to make higher-level code convenient.

Prefer adapting higher-level code to stable kernel interfaces.

---

# 7. Avoid God Objects

This is a strict rule.

Do NOT turn core structures into containers for unrelated subsystem state.

Especially avoid adding unrelated fields to:

```text
TaskControlBlock
Scheduler
VFS
NetworkManager
ScanEngine
DeviceManager
KernelHeap
CapabilityManager
```

For example, do not turn:

```cpp
TaskControlBlock
```

into:

```text
Task
+ IPC
+ networking
+ UI
+ filesystem
+ signals
+ security
+ application runtime
+ metrics
```

Prefer composition:

```cpp
struct TaskControlBlock {
    TaskContext task;
    SchedulerContext scheduler;
    IpcContext ipc;
    MemoryContext memory;
    SecurityContext security;
};
```

A new feature should normally create a dedicated context/object rather than adding unrelated fields to an existing core object.

---

# 8. Single Responsibility

Every class/module should have a clearly defined responsibility.

Examples:

```text
Scheduler
    scheduling only

IPC subsystem
    message passing only

Capability subsystem
    capability management only

VFS
    filesystem abstraction only

Network scanner
    scanning orchestration only

TCP scanner
    TCP scanning only
```

Avoid classes containing:

```text
queue management
network implementation
Lua integration
filesystem access
logging
metrics
hardware access
```

unless the architecture explicitly requires it.

---

# 9. Prefer Composition Over Large Switch Statements

Avoid large switch statements that grow every time a feature is added.

Bad:

```cpp
switch (job.type) {
    case TCP_SCAN:
    case UDP_SCAN:
    case ARP_SCAN:
    case ICMP_SCAN:
    case SERVICE_SCAN:
    case VULN_SCAN:
}
```

Prefer a strategy/handler interface when the number of implementations is expected to grow:

```cpp
class IScanHandler {
public:
    virtual Result execute(const ScanJob&, ScanResult&) noexcept = 0;
};
```

Use this principle for extensible subsystems.

Do not introduce abstraction merely for the sake of abstraction. Small, fixed kernel state machines may remain explicit.

---

# 10. C++ Standard

AuroraOS may use modern C++, but kernel code is effectively freestanding.

Do not assume the availability of the full desktop C++ standard library.

Before using a standard-library component, verify:

1. target toolchain support
2. runtime availability
3. memory requirements
4. exception requirements
5. RTTI requirements
6. code-size impact
7. deterministic behavior

---

# 11. Kernel C++ vs Host Test C++

These are different environments.

## Kernel

Prefer:

- `constexpr`
- `const`
- `noexcept`
- fixed-size containers
- static allocation
- object pools
- custom allocators
- deterministic operations
- explicit error values

Avoid depending on:

- exceptions
- RTTI
- dynamic heap allocation in hot paths
- large STL containers
- uncontrolled global initialization
- filesystem APIs
- desktop threading primitives

## Host tests

Host-side tests may use:

- GoogleTest
- sanitizers
- standard C++ library
- richer diagnostics
- exceptions where appropriate

Do not copy host-test assumptions into kernel code.

---

# 12. Memory Allocation Rules

Dynamic allocation is restricted in kernel hot paths.

Avoid:

```cpp
new
delete
malloc
free
```

inside:

- scheduler hot paths
- interrupt handlers
- IPC fast paths
- capability lookup
- context switching
- timing-critical code

Prefer:

```text
static objects
fixed-size pools
slab/object allocators
preallocated buffers
stack allocation
```

If dynamic allocation is required, document:

- why
- maximum allocation size
- failure behavior
- lifetime
- fragmentation implications
- execution context

Never introduce unbounded allocation.

---

# 13. Raw Pointers and `void*`

Raw pointers are allowed when they represent:

- hardware addresses
- non-owning references
- memory-mapped registers
- architecture state
- explicitly managed kernel objects

They must have documented ownership/lifetime semantics.

Avoid `void*` when a typed alternative exists.

Prefer:

```cpp
TaskControlBlock*
Capability*
uint8_t*
std::span<std::byte>
```

over:

```cpp
void*
```

Do not introduce `void*` merely to bypass type design.

If `void*` is required by an ABI or hardware interface, document the reason.

---

# 14. Ownership Rules

Ownership must be explicit.

For every pointer, determine whether it is:

```text
owning
non-owning
borrowed
shared
hardware address
optional
```

Do not use smart pointers automatically.

`std::unique_ptr` and `std::shared_ptr` are not automatically appropriate for kernel code.

For kernel objects, prefer project-specific ownership mechanisms when available:

```text
object tables
capability references
fixed pools
static allocation
reference counters
explicit lifetime management
```

---

# 15. RAII and Locking

Kernel synchronization must use AuroraOS-compatible RAII guards where available.

Examples:

```cpp
IrqGuard guard;
LockGuard guard(lock);
```

Do not manually pair:

```cpp
lock();
unlock();
```

unless the architecture or low-level primitive explicitly requires it.

If manual locking is unavoidable, document why RAII cannot be used.

Do not create temporary RAII guards whose lifetime ends immediately.

Bad:

```cpp
LockGuard(lock);
critical_operation();
```

Prefer:

```cpp
LockGuard guard(lock);
critical_operation();
```

---

# 16. Interrupt Safety

Interrupt context is fundamentally different from normal task context.

Never assume an interrupt handler can:

- allocate memory
- block
- sleep
- wait for a mutex
- perform long computations
- call arbitrary userspace code
- invoke blocking IPC

Keep interrupt handlers short.

Prefer:

```text
ISR
 ↓
capture minimal state
 ↓
signal/defer work
 ↓
worker/task
```

---

# 17. Scheduler Rules

Scheduler code must remain deterministic.

Do not introduce:

- unbounded loops
- heap allocation
- blocking operations
- filesystem operations
- network operations
- logging-heavy paths

into scheduler hot paths.

Any change affecting:

```text
priority
ready queues
context switching
preemption
interrupt masking
task state
```

must be reviewed for:

- starvation
- priority inversion
- race conditions
- interrupt latency
- worst-case execution time

---

# 18. TaskControlBlock Rules

`TaskControlBlock` is a critical kernel structure.

Do not add subsystem-specific fields casually.

Before adding a field, ask:

1. Is this required for task scheduling?
2. Is this required for task identity/lifetime?
3. Is this required for kernel isolation?
4. Can it live in a dedicated context structure?
5. Can it be referenced through an existing subsystem object?

If the answer is no, do not add it to TCB.

---

# 19. IPC Rules

IPC must respect capability and isolation boundaries.

Never bypass capability validation for convenience.

Do not directly expose kernel pointers to userspace.

User-provided addresses must be validated before kernel memory access.

Prefer:

```text
user pointer
    ↓
validation
    ↓
copy_from_user / safe access
    ↓
kernel buffer
    ↓
IPC
```

Avoid blindly copying from arbitrary addresses.

IPC paths should avoid:

- unbounded allocation
- unbounded message sizes
- blocking while holding global locks
- unnecessary copies

Message limits must be explicit.

---

# 20. Capability Rules

Capabilities are security boundaries.

Do not introduce global handles when a capability can represent the resource.

Bad:

```cpp
global_device_handle
```

Prefer:

```text
capability → object → operation
```

Every new kernel resource should answer:

```text
Who owns it?
Who can derive it?
Who can invoke it?
Who can revoke it?
What authority does it represent?
```

Never bypass capability checks merely because the caller is "trusted".

---

# 21. MPU/MMU Rules

Memory isolation must be preserved.

Changes involving:

```text
MPU
MMU
page tables
sandbox
address validation
task memory
stack boundaries
```

must consider:

- alignment
- permissions
- privilege level
- context switching
- cache/TLB behavior where applicable
- fault handling
- architecture differences

Do not assume ARM MPU semantics apply to RISC-V MMU code.

---

# 22. Error Handling

Kernel code should prefer explicit error handling.

Use project-defined:

```text
Error enum
Result<T, Error>
nullptr
bool
status code
```

where appropriate.

Do not introduce exceptions into kernel code unless the project explicitly enables and supports them for that subsystem.

Never silently ignore allocation, IPC, capability, hardware, or initialization failures.

Bad:

```cpp
do_operation();
return true;
```

Better:

```cpp
const auto result = do_operation();
if (!result.ok()) {
    return result.error();
}
```

---

# 23. Panic and Fatal Errors

Do not scatter:

```cpp
while (true) {}
```

throughout the kernel.

Use a centralized fatal/panic mechanism when available:

```text
Kernel::panic(...)
```

Fatal conditions should provide:

- reason
- subsystem
- optional diagnostic code
- architecture-safe behavior

Do not duplicate panic implementation in every subsystem.

---

# 24. CMake Rules

The root `CMakeLists.txt` must remain small.

Avoid adding large board-specific branches:

```cmake
if(BOARD STREQUAL ...)
elseif(...)
elseif(...)
elseif(...)
```

Board-specific configuration belongs under:

```text
boards/
config/
```

Architecture-specific configuration belongs under:

```text
arch/
```

Third-party dependencies should be isolated behind dedicated targets.

Avoid `file(GLOB ...)` for AuroraOS-owned source files.

Explicit source lists are preferred because they make changes visible and reproducible.

`file(GLOB ...)` may be used for carefully controlled vendored dependencies when necessary.

---

# 25. Kconfig Rules

Never manually edit generated files:

```text
config/autoconf.h
config/autoconf.cmake
```

Workflow:

```text
Kconfig
   ↓
genconfig.py
   ↓
generated configuration
   ↓
CMake
   ↓
build
```

If configuration changes are required:

1. modify the source Kconfig
2. regenerate configuration
3. rebuild
4. run tests

---

# 26. Third-Party Dependencies

`3rdparty/` is vendored external code.

AI agents must not modify third-party source unless explicitly instructed.

Examples:

```text
Lua
lwIP
LittleFS
Ed25519
NimBLE
GoogleTest
```

Do not "fix" third-party code by editing it directly.

Instead:

1. verify whether AuroraOS integration is wrong
2. use an adapter
3. patch through the documented vendoring mechanism
4. update the dependency when appropriate

---

# 27. Drivers

Drivers must not leak hardware-specific details into generic kernel APIs.

Prefer:

```text
Application
 ↓
Device API
 ↓
Driver interface
 ↓
HAL
 ↓
Hardware
```

Avoid board checks inside generic drivers:

```cpp
if (BOARD == ...)
```

Prefer board configuration to provide the hardware mapping.

---

# 28. Networking

Network services must not be coupled directly to kernel internals.

Prefer:

```text
lwIP
 ↓
AuroraOS network adapter
 ↓
network services
```

Do not place:

```text
scanner
firewall
service detection
Lua networking logic
```

inside the kernel scheduler or IPC implementation.

Networking code must define explicit resource limits.

---

# 29. VFS

VFS must provide an abstraction boundary.

Do not make generic VFS APIs depend directly on a specific filesystem implementation.

Prefer:

```text
VFS
├── LittleFS
├── RAMFS
├── ROMFS
└── future filesystems
```

Filesystem-specific behavior belongs to filesystem implementations.

---

# 30. Metrics and Logging

Metrics must not create circular dependencies.

Avoid:

```text
KernelHeap
 ↓
Metrics
 ↓
KernelHeap
```

Cross-cutting metrics should use:

- lightweight hooks
- event interfaces
- independent collectors
- compile-time optional instrumentation

Metrics must not make core kernel functionality depend on optional telemetry.

---

# 31. Experimental Features

Experimental features must clearly state:

```text
status
limitations
supported boards
dependencies
test coverage
security status
```

Recommended states:

```text
experimental
incubating
stable
deprecated
removed
```

Do not claim an experimental feature is production-ready.

---

# 32. Testing

Host-side tests are mandatory for testable kernel algorithms.

Typical workflow:

```bash
cmake -S tests -B build_tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tests -j$(nproc)
ctest --test-dir build_tests --output-on-failure
```

When modifying:

```text
memory
scheduler
IPC
capability
data structures
parsers
protocol logic
```

add or update tests.

Do not modify production behavior merely to make tests pass.

---

# 33. Test Categories

Prefer tests covering:

### Functional

```text
normal operation
boundary conditions
invalid input
resource exhaustion
```

### Safety

```text
buffer limits
invalid pointers
capability violations
double free
use-after-free where detectable
```

### Concurrency

```text
race conditions
priority behavior
queue consistency
interrupt/task interaction
```

### Architecture

```text
ARM
RISC-V
host
```

where applicable.

---

# 34. Sanitizers and Static Analysis

Use available tooling when modifying complex C/C++ code.

Relevant checks may include:

```text
AddressSanitizer
UndefinedBehaviorSanitizer
ThreadSanitizer where applicable
clang-tidy
cppcheck
compiler warnings
coverage
```

Host sanitizer results must not be blindly interpreted as bare-metal behavior, but they are valuable for detecting algorithmic memory errors.

---

# 35. Build Targets

Primary targets currently include:

```text
LM3S6965 QEMU
MiBand 8 / Apollo3
Nucleo-L031K6
RISC-V RV32 QEMU
```

Do not assume that code compiling for one architecture compiles for all targets.

When changing architecture-independent kernel code, check at least:

```text
host tests
primary QEMU target
```

When changing architecture-specific code, test the affected architecture.

---

# 36. Resource Budgets

AuroraOS targets constrained hardware.

Every new subsystem must consider:

```text
.text
.rodata
.data
.bss
stack
heap
IPC buffers
DMA buffers
```

For resource-constrained targets, do not introduce large static arrays without justification.

Avoid unnecessary copies.

Avoid unbounded buffers.

---

# 37. Naming

Follow the existing project's naming conventions consistently.

Do not rename large areas of the codebase merely to satisfy personal style preferences.

Before introducing a new naming convention:

1. inspect nearby code
2. follow the dominant subsystem convention
3. avoid unrelated churn

---

# 38. Comments

Comments should explain:

- why something exists
- architectural invariants
- hardware constraints
- security assumptions
- concurrency assumptions
- ownership/lifetime
- non-obvious algorithms

Do not write comments that merely repeat the code.

Bad:

```cpp
// Increment count
count++;
```

Good:

```cpp
// Must remain atomic with queue insertion because the
// scheduler may inspect ready_count from interrupt context.
```

---

# 39. API Stability

Before changing a public interface, search the entire repository for callers.

Do not assume the visible caller is the only caller.

Check:

```text
kernel
tests
apps
drivers
net
experimental
scripts
```

If an API change is necessary:

1. update all callers
2. update tests
3. update documentation
4. explain compatibility implications

---

# 40. Avoid Copy-Paste

Do not duplicate:

```text
validation
buffer copying
error handling
locking
message parsing
resource cleanup
```

If the same logic appears multiple times, consider extracting a helper.

However, do not create abstractions solely to eliminate two tiny similar functions.

Prefer clear code over premature abstraction.

---

# 41. Technical Debt Guard

Before adding new code, AI agents must ask:

1. Does this functionality belong to an existing subsystem?
2. Does this create a new dependency edge?
3. Does this add fields to a core structure?
4. Does this duplicate existing functionality?
5. Does this increase root CMake complexity?
6. Does this introduce another board-specific branch?
7. Does this make experimental code required by stable code?
8. Does this create a new global singleton?
9. Does this introduce a new ownership model?
10. Can the feature be implemented behind an existing interface?

If the change increases architectural complexity, explain why.

Do not silently increase technical debt.

---

# 42. Dependency Direction Check

Before finalizing a change, inspect new include/dependency relationships.

Avoid cycles such as:

```text
A → B → C → A
```

Especially avoid:

```text
kernel ↔ subsystem
kernel ↔ driver
stable ↔ experimental
VFS ↔ application
network ↔ UI
```

If a cycle appears necessary, introduce an interface/adapter boundary.

---

# 43. AI Modification Boundaries

Unless explicitly instructed, AI agents must not modify:

```text
3rdparty/
.github/workflows/
generated configuration
linker scripts
boot ROM assumptions
hardware calibration data
```

The following require extra caution:

```text
kernel/task.*
kernel/scheduler.*
kernel/ipc.*
kernel/cspace.*
kernel/memory.*
arch/**
boot/**
syscall/**
```

Do not make broad refactors in these areas while implementing an unrelated feature.

---

# 44. Change Scope

Prefer small, reviewable changes.

A feature change should not simultaneously:

```text
rewrite CMake
rename the kernel
reformat all files
change naming conventions
replace allocators
rewrite tests
```

unless explicitly requested.

Separate architectural refactoring from feature implementation when practical.

---

# 45. Refactoring Rules

When refactoring:

1. Preserve behavior first.
2. Add tests before large changes if coverage is insufficient.
3. Make one architectural change at a time.
4. Keep commits logically focused.
5. Do not mix formatting-only changes with functional changes.
6. Verify all affected targets.

For high-risk kernel refactors:

```text
before
 ↓
tests
 ↓
refactor
 ↓
tests
 ↓
target build
 ↓
review
```

---

# 46. Security Review Checklist

For every security-sensitive change, verify:

- [ ] Capability checks remain intact.
- [ ] No privilege boundary is bypassed.
- [ ] User pointers are validated.
- [ ] Buffer sizes are bounded.
- [ ] Integer overflow is considered.
- [ ] Resource exhaustion is handled.
- [ ] Sensitive data is not accidentally exposed.
- [ ] Untrusted input cannot trigger unbounded work.
- [ ] Experimental code is not exposed as a trusted service.
- [ ] Error paths do not accidentally grant authority.

---

# 47. Memory Safety Checklist

Before finalizing:

- [ ] No out-of-bounds access.
- [ ] No use-after-free.
- [ ] No double-free.
- [ ] No unchecked integer overflow where relevant.
- [ ] No unchecked allocation failure.
- [ ] No invalid pointer dereference.
- [ ] Buffer lengths are explicit.
- [ ] Ownership is clear.
- [ ] Lifetime is clear.
- [ ] Interrupt/task lifetime interactions are safe.

---

# 48. Concurrency Checklist

Before finalizing:

- [ ] Shared state is identified.
- [ ] Locking strategy is documented.
- [ ] Lock ordering is preserved.
- [ ] Interrupt context is considered.
- [ ] No blocking operation occurs in forbidden contexts.
- [ ] No lock is held unnecessarily.
- [ ] Atomicity requirements are understood.
- [ ] Priority inversion is considered where relevant.

---

# 49. Architecture Checklist

For architecture-dependent changes:

- [ ] ARM behavior considered.
- [ ] RISC-V behavior considered.
- [ ] Alignment requirements considered.
- [ ] Endianness assumptions checked.
- [ ] Interrupt behavior checked.
- [ ] Context switching implications checked.
- [ ] Compiler/ABI assumptions checked.
- [ ] Architecture-specific code remains isolated.

---

# 50. CMake Checklist

Before finalizing:

- [ ] Did this increase root CMake complexity?
- [ ] Can board-specific logic be moved to `boards/`?
- [ ] Can architecture-specific logic be moved to `arch/`?
- [ ] Did this introduce unnecessary `file(GLOB)`?
- [ ] Did this accidentally include third-party source?
- [ ] Did this introduce an unnecessary global compile definition?
- [ ] Are host and firmware targets still separated?

---

# 51. Documentation Checklist

If behavior or architecture changes:

- [ ] Update relevant documentation.
- [ ] Do not claim unsupported hardware works.
- [ ] Mark experimental functionality accurately.
- [ ] Avoid hard-coded test counts in long-lived documentation.
- [ ] Keep commands synchronized with the current build system.

---

# 52. Required Verification Workflow

For a normal kernel/logic change:

```text
1. Read relevant architecture documentation
2. Search for callers/usages
3. Modify minimal code
4. Add/update tests
5. Build host tests
6. Run host tests
7. Run static analysis where practical
8. Build affected target
9. Review dependency changes
10. Review security implications
11. Report exactly what was tested
```

Do not claim a test was run if it was not run.

---

# 53. Quick Verification

Host tests:

```bash
cmake -S tests -B build_tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build_tests -j$(nproc)
ctest --test-dir build_tests --output-on-failure
```

Primary QEMU target:

```bash
mkdir -p build
cd build
cmake -DBOARD=lm3s6965-qb ..
cmake --build . -j$(nproc)
```

If QEMU is available:

```bash
qemu-system-arm -M lm3s6965evb -nographic -kernel auroraOS.elf
```

Do not install dependencies or modify the development environment unnecessarily.

---

# 54. Reporting Results

When completing a task, report:

```text
Changed:
- file
- file
- file

Behavior:
- what changed

Tests:
- command
- result

Build:
- target
- result

Known limitations:
- limitation

Potential technical debt:
- item
```

Do not claim:

```text
"Everything works"
```

unless the relevant tests/builds were actually executed.

---

# 55. Final AI Self-Review

Before finalizing any change, ask:

### Architecture

- Did I preserve the microkernel boundary?
- Did I introduce a dependency cycle?
- Did I add responsibilities to a God Object?
- Did I make stable code depend on experimental code?

### Memory

- Did I introduce unnecessary dynamic allocation?
- Are pointer ownership and lifetime clear?
- Are all buffers bounded?

### Security

- Did I bypass capability checks?
- Did I weaken MPU/MMU isolation?
- Did I expose a privileged operation?

### Concurrency

- Is interrupt context safe?
- Is locking correct?
- Could this deadlock?
- Could this starve another task?

### Maintainability

- Did I duplicate logic?
- Did I increase CMake complexity?
- Did I add another board-specific branch?
- Did I create a new global singleton?
- Did I make a core struct larger?

### Testing

- Did I add/update tests?
- Did I run the relevant tests?
- Did I build the affected target?

---

# 56. Priority Levels

Use the following priorities when reporting issues.

## P0 — Critical

Examples:

```text
capability bypass
memory corruption
kernel crash
scheduler corruption
IPC isolation failure
privilege escalation
boot failure
```

Must be addressed before feature work continues.

## P1 — High

Examples:

```text
God Object
major dependency cycle
unbounded allocation
serious race condition
incorrect API semantics
large CMake architecture debt
```

Should be fixed before substantial new functionality is added.

## P2 — Medium

Examples:

```text
duplication
poor naming
local abstraction problems
missing tests
documentation drift
```

Should be addressed during normal maintenance.

## P3 — Low

Examples:

```text
minor style inconsistency
non-critical comments
small refactoring opportunities
```

Do not block unrelated development.

---

# 57. Golden Rule

The most important rule for AI agents working on AuroraOS is:

> **Do not merely make the requested code work. Make it work without making the architecture harder to maintain.**

When choosing between:

```text
quick patch
```

and:

```text
small, well-bounded architectural change
```

prefer the second when the long-term complexity difference is significant.

But do not perform large unsolicited rewrites.

---

# 58. AuroraOS Engineering Philosophy

AuroraOS should evolve toward:

```text
                 AuroraOS
                    │
          ┌─────────┴─────────┐
          │                   │
       Kernel              User Space
          │                   │
    ┌─────┼─────┐        ┌────┼────┐
    │     │     │        │    │    │
 Scheduler IPC Memory   Apps Net   UI
    │     │     │        │    │
    └─────┼─────┘        │    │
          │              │    │
        Arch/HAL         │    │
          │              │    │
       Hardware      User APIs
```

The kernel should remain small.

Subsystems should remain independently testable.

Security boundaries should be explicit.

Ownership should be explicit.

Dependencies should have a clear direction.

Experimental features should remain isolated.

And every new feature should leave the codebase at least as maintainable as it found it.

---

# 59. Final Rule for AI Agents

**Never optimize for line count.**

**Never optimize for apparent feature count.**

**Never hide architectural problems with compatibility hacks.**

**Never introduce technical debt merely to make a single task easier.**

Prefer:

```text
small interfaces
clear ownership
explicit dependencies
bounded resources
testable components
stable architecture
```

AuroraOS is an operating system, not a collection of unrelated features.

Every change should preserve that distinction.