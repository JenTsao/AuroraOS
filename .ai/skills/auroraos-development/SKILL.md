# AuroraOS C++ Coding Standards (based on the C++ Core Guidelines)

Comprehensive coding standards for modern C++ (C++17/20/23) in the context of AuroraOS, a lightweight Nordic-style operating system project. Derived directly from the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines). Enforces type safety, resource safety, immutability, clarity, and the Rule of Zero/Five while respecting AuroraOS early-stage development needs (Meson build system, QEMU debugging, FS rewrites with Doxygen, kernel drivers, and mandatory manual review of AI-generated code).

## When to Use

- Writing new C++ code in kernel modules, drivers, or low-level components
- Reviewing or refactoring existing C++ code during builds or debugging
- Making architectural decisions in AuroraOS projects
- Enforcing consistent style across the codebase (especially with Meson build files and cross_file)
- Choosing between language features (e.g., enum vs enum class, raw pointer vs smart pointer)
- AI-assisted code generation (project explicitly requires manual review and rewrite of AI snippets)

### When NOT to Use

- Non-C++ files (Shell scripts, Assembly, pure Meson)
- Legacy C codebases that cannot adopt modern C++ features
- Embedded/bare-metal contexts where specific guidelines conflict with hardware constraints (adapt selectively)
- Production use outside of learning or early-stage AuroraOS development

## Cross-Cutting Principles

These themes recur across the entire guidelines and form the foundation for AuroraOS development:

1. **RAII everywhere** (P.8, R.1, E.6, C.20): Bind resource lifetime to object lifetime — essential for kernel resource management and preventing leaks in drivers
2. **Immutability by default** (P.10, Con.1-5, ES.25): Start with const/constexpr; mutability is the exception — aligns with AuroraOS QoL optimizations and Nordic style
3. **Type safety** (P.4, I.4, ES.46-49, Enum.3): Use the type system to prevent errors at compile time — critical for strongly typed interfaces in procfs and device drivers
4. **Express intent** (P.3, F.1, NL.1-2, T.10): Names, types, and concepts should communicate purpose — use underscore_style naming consistent with AuroraOS kernels
5. **Minimize complexity** (F.2-3, ES.5, Per.4-5): Simple code is correct code — especially important in early-stage QEMU debugging
6. **Value semantics over pointer semantics** (C.10, R.3-5, F.20, C.31): Prefer returning by value and scoped objects

## Philosophy & Interfaces (P.*, I.*)

Key rules for clear, safe interfaces in AuroraOS:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **P.1** | Express ideas directly in code | Kernel interfaces must be clear and understandable |
| **P.3** | Express intent | Names and types communicate purpose (underscore_style) |
| **P.4** | Ideally, a program should be statically type safe | Strongly typed procfs and driver interfaces |
| **P.5** | Prefer compile-time checking to run-time checking | Meson build-time checks |
| **P.8** | Don't leak any resources | RAII for all kernel resources |
| **P.10** | Prefer immutable data to mutable data | Default const/constexpr in classes |
| **I.1** | Make interfaces explicit | Explicit types and ownership |
| **I.2** | Avoid non-const global variables | No global state in kernel modules |
| **I.4** | Make interfaces precisely and strongly typed | Device drivers and procfs |
| **I.11** | Never transfer ownership by a raw pointer or reference | AI code review enforces this |
| **I.23** | Keep the number of function arguments low | Simple function APIs in drivers |

### DO

```cpp
// P.10 + I.4: Immutable, strongly typed interface in AuroraOS style
struct PageFrame {
    explicit PageFrame(size_t order) noexcept
        : order_(order), flags_(0) {}

    ~PageFrame() = default;

    size_t order_ = 0;
    uint32_t flags_ = 0;  // immutable where possible
};
```

### DON'T

```cpp
// Weak interface: unclear ownership, unclear units
double boil(double* temp);

// Non-const global variable
int g_counter = 0;  // I.2 violation
```

## Functions (F.*)

Key rules for functions in AuroraOS:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **F.1** | Package meaningful operations as carefully named functions | Kernel helper functions |
| **F.2** | A function should perform a single logical operation | Single-purpose driver functions |
| **F.3** | Keep functions short and simple | QEMU debugging helpers |
| **F.4** | If a function might be evaluated at compile time, declare it constexpr | Meson and Doxygen integration |
| **F.6** | If your function must not throw, declare it noexcept | Kernel functions that cannot fail |
| **F.8** | Prefer pure functions | Pure math or lookup in drivers |
| **F.16** | For "in" parameters, pass cheaply-copied types by value and others by const& | Array parameters in drivers |
| **F.20** | For "out" values, prefer return values to output parameters | Return structs from init functions |
| **F.21** | To return multiple "out" values, prefer returning a struct | Driver return codes |
| **F.43** | Never return a pointer or reference to a local object | Scoped objects only |

### Parameter Passing

```cpp
// F.16: Cheap types by value, others by const&
void print(int x);                           // cheap: by value
void analyze(const std::string& data);       // expensive: by const&
void transform(std::string s);               // sink: by value (will move)

// F.20 + F.21: Return values, not output parameters
struct DriverInitResult {
    bool success;
    std::string message;
};

DriverInitResult init_driver();   // GOOD: return struct
```

### Pure Functions and constexpr

```cpp
// F.4 + F.8: Pure, constexpr where possible
constexpr size_t align_up(size_t value, size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

static_assert(align_up(100, 64) == 128);
```

### Anti-Patterns

- Returning T&& from functions (F.45)
- Using va_arg / C-style variadics (F.55)
- Capturing by reference in lambdas passed to other threads (F.53)
- Returning const T which inhibits move semantics (F.49)

## Classes & Class Hierarchies (C.*)

Key rules for classes in AuroraOS:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **C.2** | Use class if invariant exists; struct if data members vary independently | Drivers with invariants |
| **C.9** | Minimize exposure of members | Private members only |
| **C.20** | If you can avoid defining default operations, do (Rule of Zero) | Most kernel classes |
| **C.21** | If you define or =delete any copy/move/destructor, handle them all (Rule of Five) | Resource-managing classes |
| **C.35** | Base class destructor: public virtual or protected non-virtual | Abstract drivers |
| **C.41** | A constructor should create a fully initialized object | Driver constructors |
| **C.46** | Declare single-argument constructors explicit | Constructor APIs |
| **C.67** | A polymorphic class should suppress public copy/move | Interface classes |
| **C.128** | Virtual functions: specify exactly one of virtual, override, or final | Virtual driver methods |

### Rule of Zero

```cpp
// C.20: Let the compiler generate special members (AuroraOS kernel common)
class Driver {
public:
    virtual ~Driver() = default;
    virtual void init() = 0;
    virtual void cleanup() = 0;

private:
    // no special members needed
};
```

### Rule of Five

```cpp
// C.21: If you must manage a resource, define all five
class PageAllocator {
public:
    explicit PageAllocator(size_t size) noexcept
        : data_(std::make_unique<uint8_t[]>(size)), size_(size) {}

    ~PageAllocator() = default;

    PageAllocator(const PageAllocator& other)
        : data_(std::make_unique<uint8_t[]>(other.size_)), size_(other.size_) {
        std::copy_n(other.data_.get(), size_, data_.get());
    }

    PageAllocator& operator=(const PageAllocator& other) {
        if (this != &other) {
            auto new_data = std::make_unique<uint8_t[]>(other.size_);
            std::copy_n(other.data_.get(), other.size_, new_data.get());
            data_ = std::move(new_data);
            size_ = other.size_;
        }
        return *this;
    }

    PageAllocator(PageAllocator&&) noexcept = default;
    PageAllocator& operator=(PageAllocator&&) noexcept = default;

private:
    std::unique_ptr<uint8_t[]> data_;
    size_t size_;
};
```

### Class Hierarchy

```cpp
// C.35 + C.128: Virtual destructor, use override
class DriverInterface {
public:
    virtual ~DriverInterface() = default;
    virtual bool init() const = 0;  // C.121: pure interface
};

class SerialDriver : public DriverInterface {
public:
    bool init() const override { return true; }
};
```

### Anti-Patterns

- Calling virtual functions in constructors/destructors (C.82)
- Using memset/memcpy on non-trivial types (C.90)
- Providing different default arguments for virtual function and overrider (C.140)
- Making data members const or references, which suppresses move/copy (C.12)

## Resource Management (R.*)

Key rules for resource management in AuroraOS:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **R.1** | Manage resources automatically using RAII | All kernel resources |
| **R.3** | A raw pointer (T*) is non-owning | Observer pointers |
| **R.5** | Prefer scoped objects; don't heap-allocate unnecessarily | Driver objects |
| **R.10** | Avoid malloc()/free() | Kernel avoids C heap |
| **R.11** | Avoid calling new and delete explicitly | Smart pointers only |
| **R.20** | Use unique_ptr or shared_ptr to represent ownership | Driver ownership |
| **R.21** | Prefer unique_ptr over shared_ptr unless sharing ownership | Exclusive ownership |
| **R.22** | Use make_shared() to make shared_ptrs | Shared ownership only when needed |

### Smart Pointer Usage

```cpp
// R.11 + R.20 + R.21: RAII with smart pointers
auto page = std::make_unique<PageFrame>(4);  // unique ownership
auto cache  = std::make_shared<DriverCache>(1024);  // shared ownership

// R.3: Raw pointer = non-owning observer
void render(const Widget* w) {  // does NOT own w
    if (w) w->draw();
}

render(page.get());
```

### RAII Pattern

```cpp
// R.1: Resource acquisition is initialization
class FileHandle {
public:
    explicit FileHandle(const std::string& path)
        : handle_(std::fopen(path.c_str(), "r")) {
        if (!handle_) throw std::runtime_error("Failed to open: " + path);
    }

    ~FileHandle() {
        if (handle_) std::fclose(handle_);
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            if (handle_) std::fclose(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

private:
    std::FILE* handle_;
};
```

### Anti-Patterns

- Naked new/delete (R.11)
- malloc()/free() in C++ code (R.10)
- Multiple resource allocations in a single expression (R.13 -- exception safety hazard)
- shared_ptr where unique_ptr suffices (R.21)

## Expressions & Statements (ES.*)

Key rules for expressions and statements:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **ES.5** | Keep scopes small | Nested driver code |
| **ES.20** | Always initialize an object | Driver members |
| **ES.23** | Prefer {} initializer syntax | Construction |
| **ES.25** | Declare objects const or constexpr unless modification is intended | Immutable drivers |
| **ES.28** | Use lambdas for complex initialization of const variables | Meson configuration |
| **ES.45** | Avoid magic constants; use symbolic constants | Driver constants |
| **ES.46** | Avoid narrowing/lossy arithmetic conversions | Size calculations |
| **ES.47** | Use nullptr rather than 0 or NULL | Pointer checks |
| **ES.48** | Avoid casts | Explicit casts only |
| **ES.50** | Don't cast away const | Const-correct code |

### Initialization

```cpp
// ES.20 + ES.23 + ES.25: Always initialize, prefer {}, default to const
const size_t max_order{4};
const std::string name{"serial0"};
const std::vector<int> primes{2, 3, 5, 7, 11};

// ES.28: Lambda for complex const initialization
const auto config = [&] {
    DriverConfig c;
    c.timeout = std::chrono::seconds{30};
    c.order = max_order;
    c.verbose = debug_mode;
    return c;
}();
```

### Anti-Patterns

- Uninitialized variables (ES.20)
- Using 0 or NULL as pointer (ES.47 -- use nullptr)
- C-style casts (ES.48 -- use static_cast, const_cast, etc.)
- Casting away const (ES.50)
- Magic numbers without named constants (ES.45)
- Mixing signed and unsigned arithmetic (ES.100)
- Reusing names in nested scopes (ES.12)

## Error Handling (E.*)

Key rules for error handling:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **E.1** | Develop an error-handling strategy early in a design | Driver init paths |
| **E.2** | Throw an exception to signal that a function can't perform its assigned task | Failure signaling |
| **E.6** | Use RAII to prevent leaks | Resource errors |
| **E.12** | Use noexcept when throwing is impossible or unacceptable | Safe kernel functions |
| **E.14** | Use purpose-designed user-defined types as exceptions | Custom driver errors |
| **E.15** | Throw by value, catch by reference | Exception safety |
| **E.16** | Destructors, deallocation, and swap must never fail | RAII guarantees |
| **E.17** | Don't try to catch every exception in every function | Specific catches only |

### Exception Hierarchy

```cpp
// E.14 + E.15: Custom exception types, throw by value, catch by reference
class DriverError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SerialError : public DriverError {
public:
    SerialError(const std::string& msg, int code)
        : DriverError(msg), status_code(code) {}
    int status_code;
};

void init_serial(const std::string& port) {
    // E.2: Throw to signal failure
    throw SerialError("port not found", 404);
}

void run() {
    try {
        init_serial("/dev/tty0");
    } catch (const SerialError& e) {
        log_error(e.what(), e.status_code);
    } catch (const DriverError& e) {
        log_error(e.what());
    }
    // E.17: Don't catch everything here -- let unexpected errors propagate
}
```

### Anti-Patterns

- Throwing built-in types like int or string literals (E.14)
- Catching by value (slicing risk) (E.15)
- Empty catch blocks that silently swallow errors
- Using exceptions for flow control (E.3)
- Error handling based on global state like errno (E.28)

## Constants & Immutability (Con.*)

All rules:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **Con.1** | By default, make objects immutable | Default const |
| **Con.2** | By default, make member functions const | Read-only drivers |
| **Con.3** | By default, pass pointers and references to const | Observer interfaces |
| **Con.4** | Use const for values that don't change after construction | Driver members |
| **Con.5** | Use constexpr for values computable at compile time | Meson constants |

```cpp
// Con.1 through Con.5: Immutability by default
class DriverSensor {
public:
    explicit DriverSensor(std::string id) : id_(std::move(id)) {}

    // Con.2: const member functions by default
    const std::string& id() const { return id_; }
    double last_reading() const { return reading_; }

    // Only non-const when mutation is required
    void record(double value) { reading_ = value; }

private:
    const std::string id_;  // Con.4: never changes after construction
    double reading_{0.0};
};

// Con.3: Pass by const reference
void display(const DriverSensor& s) {
    std::cout << s.id() << ": " << s.last_reading() << '\n';
}

// Con.5: Compile-time constants
constexpr double PI = 3.14159265358979;
constexpr size_t MAX_PAGES = 256;
```

## Concurrency & Parallelism (CP.*)

Key rules for concurrency (limited in AuroraOS early stage):

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **CP.2** | Avoid data races | Threaded drivers |
| **CP.3** | Minimize explicit sharing of writable data | Shared state |
| **CP.4** | Think in terms of tasks, rather than threads | Task-based design |
| **CP.8** | Don't use volatile for synchronization | Hardware I/O only |
| **CP.20** | Use RAII, never plain lock()/unlock() | Scoped locks |
| **CP.21** | Use std::scoped_lock to acquire multiple mutexes | Deadlock-free |
| **CP.22** | Never call unknown code while holding a lock | Safe locking |
| **CP.42** | Don't wait without a condition | Proper waiting |
| **CP.44** | Remember to name your lock_guard's and unique_lock's | Named locks |
| **CP.100** | Don't use lock-free programming unless you absolutely have to | Early-stage avoidance |

### Safe Locking

```cpp
// CP.20 + CP.44: RAII locks, always named
class ThreadSafeQueue {
public:
    void push(int value) {
        std::lock_guard<std::mutex> lock(mutex_);  // CP.44: named!
        queue_.push(value);
        cv_.notify_one();
    }

    int pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        // CP.42: Always wait with a condition
        cv_.wait(lock, [this] { return !queue_.empty(); });
        const int value = queue_.front();
        queue_.pop();
        return value;
    }

private:
    std::mutex mutex_;             // CP.50: mutex with its data
    std::condition_variable cv_;
    std::queue<int> queue_;
};
```

### Multiple Mutexes

```cpp
// CP.21: std::scoped_lock for multiple mutexes (deadlock-free)
void transfer(DriverAccount& from, DriverAccount& to, double amount) {
    std::scoped_lock lock(from.mutex_, to.mutex_);
    from.balance_ -= amount;
    to.balance_ += amount;
}
```

### Anti-Patterns

- volatile for synchronization (CP.8 -- it's for hardware I/O only)
- Detaching threads (CP.26 -- lifetime management becomes nearly impossible)
- Unnamed lock guards: std::lock_guard<std::mutex>(m); destroys immediately (CP.44)
- Holding locks while calling callbacks (CP.22 -- deadlock risk)
- Lock-free programming without deep expertise (CP.100)

## Templates & Generic Programming (T.*)

Key rules for templates:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **T.1** | Use templates to raise the level of abstraction | Generic drivers |
| **T.2** | Use templates to express algorithms for many argument types | Driver algorithms |
| **T.10** | Specify concepts for all template arguments | C++20+ concepts |
| **T.11** | Use standard concepts whenever possible | Ranges and algorithms |
| **T.13** | Prefer shorthand notation for simple concepts | Simple templates |
| **T.43** | Prefer using over typedef | Modern C++ |
| **T.120** | Use template metaprogramming only when you really need to | Compile-time only |
| **T.144** | Don't specialize function templates (overload instead) | Overloading |

### Concepts (C++20)

```cpp
#include <concepts>

// T.10 + T.11: Constrain templates with standard concepts
template<std::integral T>
T gcd(T a, T b) {
    while (b != 0) {
        a = std::exchange(b, a % b);
    }
    return a;
}

// T.13: Shorthand concept syntax
void sort(std::ranges::random_access_range auto& range) {
    std::ranges::sort(range);
}

// Custom concept for domain-specific constraints
template<typename T>
concept Serializable = requires(const T& t) {
    { t.serialize() } -> std::convertible_to<std::string>;
};

template<Serializable T>
void save(const T& obj, const std::string& path);
```

### Anti-Patterns

- Unconstrained templates in visible namespaces (T.47)
- Specializing function templates instead of overloading (T.144)
- Template metaprogramming where constexpr suffices (T.120)
- typedef instead of using (T.43)

## Standard Library (SL.*)

Key rules for the Standard Library:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **SL.1** | Use libraries wherever possible | Prefer std over custom |
| **SL.2** | Prefer the standard library to other libraries | Standard over third-party |
| **SL.con.1** | Prefer std::array or std::vector over C arrays | Driver data |
| **SL.con.2** | Prefer std::vector by default | Dynamic driver data |
| **SL.str.1** | Use std::string to own character sequences | Driver strings |
| **SL.str.2** | Use std::string_view to refer to character sequences | Read-only views |
| **SL.io.50** | Avoid endl (use '\n' -- endl forces a flush) | Console output |

```cpp
// SL.con.1 + SL.con.2: Prefer vector/array over C arrays
const std::array<int, 4> fixed_data{1, 2, 3, 4};
std::vector<std::string> dynamic_data;

// SL.str.1 + SL.str.2: string owns, string_view observes
std::string build_greeting(std::string_view name) {
    return "Hello, " + std::string(name) + "!";
}

// SL.io.50: Use '\n' not endl
std::cout << "result: " << value << '\n';
```

## Enumerations (Enum.*)

Key rules for enumerations:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **Enum.1** | Prefer enumerations over macros | Driver constants |
| **Enum.3** | Prefer enum class over plain enum | Strongly typed |
| **Enum.5** | Don't use ALL_CAPS for enumerators | AuroraOS style |
| **Enum.6** | Avoid unnamed enumerations | Named only |

```cpp
// Enum.3 + Enum.5: Scoped enum, no ALL_CAPS
enum class Color { red, green, blue };
enum class LogLevel { debug, info, warning, error };

// BAD: plain enum leaks names, ALL_CAPS clashes with macros
enum { RED, GREEN, BLUE };           // Enum.3 + Enum.5 + Enum.6 violation
#define MAX_SIZE 100                  // Enum.1 violation -- use constexpr
```

## Source Files & Naming (SF.*, NL.*)

Key rules for source files and naming:

| Rule | Summary | AuroraOS Applicable Scenario |
|------|---------|-----------------------------|
| **SF.1** | Use .cpp for code files and .h for interface files | AuroraOS structure |
| **SF.7** | Don't write using namespace at global scope in a header | Header safety |
| **SF.8** | Use #include guards for all .h files | Header protection |
| **SF.11** | Header files should be self-contained | Include everything needed |
| **NL.5** | Avoid encoding type information in names (no Hungarian notation) | Clear names |
| **NL.8** | Use a consistent naming style | underscore_style |
| **NL.9** | Use ALL_CAPS for macro names only | Constants |
| **NL.10** | Prefer underscore_style names | Kernel style |

### Header Guard

```cpp
// SF.8: Include guard (or #pragma once)
#ifndef AURORAOS_KERNEL_DRIVER_SERIAL_H
#define AURORAOS_KERNEL_DRIVER_SERIAL_H

// SF.11: Self-contained -- include everything this header needs
#include <string>
#include <vector>
#include <memory>

namespace auroraos::kernel::driver {

class SerialDriver {
public:
    explicit SerialDriver(std::string name);
    const std::string& name() const;

private:
    std::string name_;
};

}  // namespace auroraos::kernel::driver
#endif
```
