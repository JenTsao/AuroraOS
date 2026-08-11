# Security Policy

Security is a fundamental design goal of AuroraOS. The system relies on a capability-based microkernel architecture to enforce strict isolation and least privilege.

## Supported Versions

Currently, AuroraOS is in an early development phase (Cycle 0/1) and is not yet recommended for production use where critical security guarantees are required.

## Reporting a Vulnerability

If you discover a security vulnerability within AuroraOS, please do not disclose it publicly.

1. Send an email to the security team (placeholder: security@auroraos.org).
2. Include a detailed description of the vulnerability, steps to reproduce, and potential impact.
3. We will acknowledge receipt of your report within 48 hours and provide an estimated timeline for resolution.

## Core Security Principles

- **Capability-Based Access:** No access is granted implicitly. All accesses to objects, memory, and devices require explicit capabilities.
- **Microkernel Isolation:** The kernel minimalistically manages mechanisms, moving as much logic as possible to isolated userspace services.
- **Principle of Least Privilege:** Components only possess the capabilities necessary for their specific tasks.
- **Memory Safety:** AuroraOS uses MPU/MMU for hardware-enforced memory isolation and modern C++ features (RAII, smart pointers) to mitigate memory errors.

For a detailed roadmap of the security architecture, refer to `auroraos_cybersec_plan.md`.
