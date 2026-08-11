# Contributing to AuroraOS

Welcome to the AuroraOS project! This document outlines the contribution guidelines, branching rules, commit standards, and the C++ coding standards (based on the C++ Core Guidelines) that all developers and AI agents must follow.

## 1. Branching Model

We follow a structured branching model:
- `main`: The stable branch, representing the latest release.
- `develop`: The main integration branch for the next release.
- `feature/*`: For developing new features. Branch off from `develop` and merge back into `develop`.
- `bugfix/*`: For fixing bugs in `develop` or `main`.
- `release/*`: For release preparation.

## 2. Commit Message Convention

Commits must follow the Conventional Commits format to clearly describe the changes:

**Format:**
```
<type>(<scope>): <subject>
```

**Types:**
- `feat`: A new feature
- `fix`: A bug fix
- `docs`: Documentation only changes
- `style`: Changes that do not affect the meaning of the code (white-space, formatting, etc.)
- `refactor`: A code change that neither fixes a bug nor adds a feature
- `perf`: A code change that improves performance
- `test`: Adding missing tests or correcting existing tests
- `chore`: Changes to the build process or auxiliary tools and libraries

**Example:**
- `fix(ipc): fix message size check` (Correct)
- `Update code` (Incorrect)

## 3. Pull Request Process

1. Ensure your code strictly follows the coding standards.
2. Add or update tests for your changes.
3. Update relevant documentation (e.g., `ARCHITECTURE.md`, `README.md`).
4. Submit a PR against the `develop` branch.
5. The PR must pass CI checks (builds, static analysis, tests).
6. Required code review focusing on:
   - Architecture impact
   - Security impact
   - Performance impact
   - Test coverage
   - Documentation synchronization

---

## 4. AuroraOS C++ Coding Standards

AuroraOS is written in modern C++ (C++17/20/23). All kernel modules, drivers, and low-level components must adhere to the following standards, derived from the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines).

### 4.1 Cross-Cutting Principles

1. **RAII everywhere:** Bind resource lifetime to object lifetime. Essential for kernel resource management and preventing leaks in drivers.
2. **Immutability by default:** Start with `const`/`constexpr`; mutability is the exception.
3. **Type safety:** Use the type system to prevent errors at compile time (e.g., strongly typed interfaces in procfs and device drivers).
4. **Express intent:** Names, types, and concepts should communicate purpose. Use `underscore_style` naming consistent with AuroraOS kernels.
5. **Minimize complexity:** Simple code is correct code. Avoid unnecessary abstractions.
6. **Value semantics over pointer semantics:** Prefer returning by value and scoped objects.

### 4.2 Philosophy & Interfaces

- **P.8:** Don't leak any resources (Use RAII).
- **P.10:** Prefer immutable data to mutable data.
- **I.2:** Avoid non-const global variables. No global state in kernel modules.
- **I.4:** Make interfaces precisely and strongly typed.
- **I.11:** Never transfer ownership by a raw pointer or reference.

**Example (Good):**
```cpp
struct PageFrame {
    explicit PageFrame(size_t order) noexcept
        : order_(order), flags_(0) {}
    ~PageFrame() = default;

    size_t order_ = 0;
    uint32_t flags_ = 0;  // immutable where possible
};
```

### 4.3 Functions

- **F.2:** A function should perform a single logical operation.
- **F.4:** If a function might be evaluated at compile time, declare it `constexpr`.
- **F.6:** If your function must not throw, declare it `noexcept`.
- **F.16:** For "in" parameters, pass cheaply-copied types by value and others by `const&`.
- **F.20/F.21:** For "out" values, prefer return values (returning structs) to output parameters.

### 4.4 Classes & Resource Management

- **Rule of Zero (C.20):** If you can avoid defining default operations (copy, move, destructor), do so. Let the compiler generate them.
- **Rule of Five (C.21):** If you define or delete any copy/move/destructor, handle them all.
- **R.1:** Manage resources automatically using RAII.
- **R.3:** A raw pointer (`T*`) is non-owning. Use it as an observer pointer.
- **R.10 / R.11:** Avoid `malloc()`, `free()`, `new`, and `delete`. Use smart pointers or scoped objects.
- **R.20:** Use `std::unique_ptr` or `std::shared_ptr` to represent ownership, preferring `std::unique_ptr`.

### 4.5 Variables & Initialization

- **ES.20:** Always initialize an object.
- **ES.23:** Prefer `{}` initializer syntax.
- **ES.25:** Declare objects `const` or `constexpr` unless modification is intended.
- **ES.45:** Avoid magic constants; use symbolic constants.
- **ES.47:** Use `nullptr` rather than `0` or `NULL`.

### 4.6 Error Handling

- **E.2:** Throw an exception (if exceptions are enabled for the subsystem) or return a Result type to signal that a function can't perform its assigned task.
- **E.6:** Use RAII to prevent leaks on failure.
- **E.12:** Use `noexcept` when throwing is impossible or unacceptable.
- **E.15:** Throw by value, catch by reference.

### 4.7 Standard Library & Enumerations

- **SL.1 / SL.2:** Use the standard library wherever possible (e.g., `std::array`, `std::vector`, `std::string`, `std::string_view`) where bare-metal constraints allow.
- **Enum.3:** Prefer `enum class` over plain `enum` for strong typing.
- **Enum.5:** Don't use ALL_CAPS for enumerators. Use `underscore_style` (e.g., `enum class Color { red, green, blue };`).

### 4.8 Concurrency

- **CP.20:** Use RAII for locks (e.g., `std::lock_guard`, `std::unique_lock`). Never use plain `lock()`/`unlock()`.
- **CP.21:** Use `std::scoped_lock` to acquire multiple mutexes to avoid deadlocks.
- **CP.44:** Always name your lock guards (e.g., `std::lock_guard<std::mutex> lock(mutex_);`).

### 4.9 Naming

- **SF.1:** Use `.cpp` for code files and `.h` for interface files.
- **SF.8:** Use `#include` guards for all `.h` files.
- **NL.8 / NL.10:** Use a consistent naming style. AuroraOS prefers `underscore_style` for variables, functions, and files. Use `PascalCase` for classes/structs where appropriate.
- **NL.9:** Use ALL_CAPS for macro names only.
