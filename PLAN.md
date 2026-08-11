# AuroraOS Development Plan

AuroraOS is developed in a phased approach, ensuring that technical debt is minimized, and a solid engineering foundation is established before complexity increases.

## 1. Six-Cycle Roadmap

### Cycle 0: Project Governance (Current Phase)
**Goal:** Establish professional engineering environments.
- Directory standardization
- Document system creation
- AI development framework
- Build and test system configuration

### Cycle 1: July Kernel Foundation
**Goal:** Build the core kernel mechanisms.
- Boot processes and early hardware initialization
- Hardware Abstraction Layer (HAL)
- Memory management
- Task and Scheduler system

### Cycle 2: Kernel IPC and Security
**Goal:** Establish the microkernel communication and capability model.
- Inter-Process Communication (IPC)
- Capability-based security model
- System Call (Syscall) interfaces

### Cycle 3: Drivers and Subsystems
**Goal:** Enable hardware interaction.
- Essential drivers (UART, GPIO, Timers)
- Virtual File System (VFS)
- Device management

### Cycle 4: System Services and Runtime
**Goal:** Move complexity to userspace.
- Network services
- File system implementations
- Userspace runtime environments (C/Rust)

### Cycle 5: Applications and UI
**Goal:** User interaction and complex apps.
- Basic UI framework
- System utilities and applications

## 2. Architectural Goals

- **Microkernel Design:** Keep the kernel small and focused on mechanisms, not policies.
- **Capability-Based Security:** Ensure all resource accesses are explicitly authorized.
- **Modularity:** Ensure strict dependency direction and avoid circular dependencies.
- **AI-Assisted Ecosystem:** Build the project to be easily maintained and extended by both human developers and AI agents.

## 3. Technology Stack

- **Kernel:** Modern C++ (C++17/20) and Architecture-specific Assembly
- **Build System:** CMake
- **Target Architectures:** ARM Cortex-M, RISC-V RV32, x86_64 (Simulation/QEMU)
- **Testing:** GoogleTest for host-side unit tests, QEMU for integration tests
