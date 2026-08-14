# Security Guide

This directory documents AuroraOS security subsystems and the security-hardening
roadmap defined in `auroraos_cybersec_plan.md`.

## Scope

The stable `security/` module is planned to consolidate the security-related
capabilities that currently live across `net/`, `services/`, and `apps/`,
including:

- `security_monitor` — runtime integrity and anomaly monitoring
- `watchdog` — kernel/hardware fault supervision
- `audit_engine` — audit logging and event tracing
- `syscall_validator` — syscall argument and capability validation
- stealth identity — MAC/OUI spoofing, iBeacon spoofing, DHCP fingerprint camouflage

Until the stable `security/` module is introduced, these components remain in
their current locations and must not be moved into stable kernel directories
merely to ease compilation (see AGENTS.md, stable vs experimental boundaries).

## Status

Placeholders only. Implementation tracked under the cybersec plan Phase 0 to 3.
