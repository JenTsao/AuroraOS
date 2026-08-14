# AuroraOS Compliance Baseline

> **Purpose**: This document is the single source of truth for AuroraOS engineering
> compliance requirements.  It is parsed by the `compliance-check` CI job
> (`scripts/check_compliance.py`); every `CHECK:` line is a machine-verifiable rule.
>
> Statuses: `PASS` (enforced by CI), `WARN` (known gap, tracked), `TODO` (not yet wired).

---

## 1. Build Integrity

| ID | Rule | CI Job | Status |
|----|------|--------|--------|
| B-01 | Root `CMakeLists.txt` MUST be <= 300 lines | `compliance-check` | PASS |
| B-02 | Board-specific config MUST live under `boards/*/board.cmake` | `compliance-check` | PASS |
| B-03 | Arch-specific flags/sources MUST live under `arch/*/arch.cmake` | `compliance-check` | PASS |
| B-04 | `AURORA_FB_CHUNK_HEIGHT` defined in exactly one canonical location (`ui/ui_config.hpp`) | `compliance-check` | PASS |
| B-05 | All 4 firmware targets MUST compile without errors | `build-*` jobs | PASS |
| B-06 | `services/ui/` prototype directory MUST NOT exist | `compliance-check` | PASS |
| B-07 | `tests/CMakeLists.txt` MUST NOT reference deleted `services/ui/` sources | `compliance-check` | PASS |

<!-- CHECK:B-01:line_count:CMakeLists.txt:300 -->
<!-- CHECK:B-04:single_define:ui/ui_config.hpp:AURORA_FB_CHUNK_HEIGHT -->
<!-- CHECK:B-06:path_absent:services/ui -->
<!-- CHECK:B-07:content_absent:tests/CMakeLists.txt:services/ui/ -->

---

## 2. Code Encoding & Formatting

| ID | Rule | CI Job | Status |
|----|------|--------|--------|
| E-01 | All tracked C/C++ source files MUST be UTF-8 without BOM | `compliance-check` | PASS |
| E-02 | No GBK/UTF-16 encoded files in tracked C/C++/CMake paths | `compliance-check` | PASS |
| E-03 | `.clang-format` MUST exist at repository root | `compliance-check` | PASS |
| E-04 | `.editorconfig` MUST exist at repository root | `compliance-check` | PASS |
| E-05 | No double-blank-line runs (>= 3 consecutive blank lines) in kernel core headers | `compliance-check` | WARN |

<!-- CHECK:E-03:file_exists:.clang-format -->
<!-- CHECK:E-04:file_exists:.editorconfig -->

---

## 3. Static Analysis Gates

| ID | Rule | CI Job | Status |
|----|------|--------|--------|
| S-01 | `clang-tidy` with `--warnings-as-errors='*'` MUST exit 0 | `static-analysis` | PASS |
| S-02 | `cppcheck --error-exitcode=1` MUST exit 0 (pipe uses `pipefail`) | `cppcheck` | PASS |
| S-03 | No `bugprone-*` diagnostics in kernel or net subsystems | `static-analysis` | PASS |
| S-04 | No `performance-*` diagnostics in hot-path scheduler code | `static-analysis` | PASS |

---

## 4. Testing Requirements

| ID | Rule | CI Job | Status |
|----|------|--------|--------|
| T-01 | Host unit test count MUST be >= 221 | `compliance-check` | PASS |
| T-02 | ASAN + UBSAN sanitizer build MUST exit 0 | `sanitize` | PASS |
| T-03 | LibFuzzer 30-second run MUST produce zero crash/OOM/timeout artifacts | `fuzzing` | PASS |
| T-04 | QEMU HIL boot smoke test MUST pass for LM3S6965 | `build-lm3s6965` | PASS |
| T-05 | Tests that are compiled MUST match firmware sources (no orphan test sources) | `compliance-check` | PASS |

<!-- CHECK:T-01:test_count_min:tests/CMakeLists.txt:221 -->
<!-- CHECK:T-05:content_absent:tests/CMakeLists.txt:services/ui/ -->

---

## 5. Architecture Invariants

| ID | Rule | CI Job | Status |
|----|------|--------|--------|
| A-01 | `experimental/` MUST NOT be `#include`d by stable kernel headers | `compliance-check` | WARN |
| A-02 | `kernel/` MUST NOT `#include` UI, application, or network headers | `compliance-check` | WARN |
| A-03 | `TaskControlBlock` MUST NOT gain new unrelated subsystem fields without review | manual review | TODO |
| A-04 | Interrupt handlers MUST NOT call `malloc`/`new`/blocking IPC | manual review | TODO |

<!-- CHECK:A-01:no_include_from:kernel:experimental -->
<!-- CHECK:A-02:no_include_from:kernel:ui -->
<!-- CHECK:A-02:no_include_from:kernel:apps -->

---

## 6. Security & Memory Safety

| ID | Rule | CI Job | Status |
|----|------|--------|--------|
| M-01 | No raw `new`/`delete` in scheduler hot paths | `static-analysis` | PASS |
| M-02 | User pointers MUST go through validation before kernel memory access | manual review | TODO |
| M-03 | Capability checks MUST NOT be bypassed | manual review | TODO |
| M-04 | No `|| true` or error-silencing in security-critical shell steps | `compliance-check` | PASS |

<!-- CHECK:M-04:content_absent:scripts/check_compliance.py:|| true -->

---

## 7. Known Gaps (WARN / TODO)

These items are tracked but not yet enforced as blocking CI gates:

| ID | Description | Target |
|----|-------------|--------|
| A-01 | Automatic detection of `experimental/` -> stable kernel includes | next cycle |
| A-02 | Automatic detection of kernel -> UI/app includes | next cycle |
| A-03 | TCB field audit automation | manual |
| A-04 | ISR `malloc` detection via static analysis | next cycle |
| E-05 | Automated double-blank-line check on kernel core headers | next cycle |
| M-02 | User-pointer validation coverage | manual |
| M-03 | Capability bypass audit | manual |
