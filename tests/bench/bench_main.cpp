// =============================================================================
// tests/bench/bench_main.cpp — AuroraOS host microbenchmarks
//
// Measures kernel hot paths natively on the host:
//   - scheduler: ready-queue remove+push, full schedule() selection/rotation
//   - IPC:       send/receive fast path, blocking call/receive/reply roundtrip
//   - cspace:    capability lookup, insert+delete (retain/release)
//   - heap:      TLSF KernelHeap 64-byte allocate+deallocate
//
// Methodology (keeps numbers comparable across CI runs):
//   * warmup = iterations / 2, discarded;
//   * N independent samples, each timing `iterations` operations with
//     std::chrono::steady_clock;
//   * the reported ns/op is the MEDIAN of the per-sample ratios, plus the
//     minimum and the coefficient of variation, so one noisy sample cannot
//     move the headline number;
//   * result values are folded into a volatile sink so the optimizer cannot
//     delete the measured work.
//
// There are deliberately no thresholds here. The repository has no historical
// baseline yet; this program records numbers only. See README / perf.yml for
// how thresholds should be introduced later.
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// tests/stubs/ is first on the include path, so this resolves to the signal-
// macro-safe wrapper around kernel/task/task.hpp, and arch_api.hpp resolves to
// the host stub.
#include "task.hpp"
#include "cspace.hpp"
#include "ipc.hpp"
#include "memory.hpp"

#if defined(__clang__)
#define AURORA_COMPILER_NAME "clang " __clang_version__
#elif defined(__GNUC__)
#define AURORA_COMPILER_NAME "gcc " __VERSION__
#elif defined(_MSC_VER)
#define AURORA_COMPILER_NAME "msvc"
#else
#define AURORA_COMPILER_NAME "unknown compiler"
#endif

namespace {

// ---------------------------------------------------------------------------
// Static task stacks. create_task() only fabricates a thread stack frame with
// Arch::init_thread_stack (a host stub), so the contents are never executed.
// ---------------------------------------------------------------------------
constexpr uint32_t kStackWords = 256;
alignas(8) uint32_t g_stack0[kStackWords];
alignas(8) uint32_t g_stack1[kStackWords];

// 1 MiB heap region for the TLSF benchmark. Large enough that splitting and
// immediate coalescing exercise the normal path instead of hitting OOM.
alignas(8) uint8_t g_heap_region[1024 * 1024];

void bench_task_entry() {}

// Volatile sink: the optimizer cannot prove it dead, so every measured
// operation stays in the generated code.
volatile uint64_t g_sink = 0;

// ---------------------------------------------------------------------------
// Timing harness
// ---------------------------------------------------------------------------
struct BenchResult {
    std::string name;
    uint64_t iterations;
    uint32_t samples;
    double median_ns;
    double min_ns;
    double max_ns;
    double cv_percent;
    double ops_per_sec;
};

template <typename Fn>
BenchResult run_benchmark(const char* name, uint64_t iterations, uint32_t samples, Fn&& op) {
    // Warmup: caches, branch predictors and the scheduler/heap state settle
    // before the first timed sample.
    const uint64_t warmup_iterations = iterations / 2 + 1;
    for (uint64_t i = 0; i < warmup_iterations; ++i) {
        g_sink ^= op();
    }

    std::vector<double> ns_per_op;
    ns_per_op.reserve(samples);
    for (uint32_t s = 0; s < samples; ++s) {
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t value = 0;
        for (uint64_t i = 0; i < iterations; ++i) {
            value += op();
        }
        const auto t1 = std::chrono::steady_clock::now();
        g_sink ^= value;

        const double total_ns =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ns_per_op.push_back(total_ns / static_cast<double>(iterations));
    }

    std::sort(ns_per_op.begin(), ns_per_op.end());
    const double median = ns_per_op[ns_per_op.size() / 2];
    const double min_ns = ns_per_op.front();
    const double max_ns = ns_per_op.back();

    double mean = 0.0;
    for (double v : ns_per_op) {
        mean += v;
    }
    mean /= static_cast<double>(ns_per_op.size());

    double variance = 0.0;
    for (double v : ns_per_op) {
        variance += (v - mean) * (v - mean);
    }
    variance /= static_cast<double>(ns_per_op.size());
    const double cv = (mean > 0.0) ? (std::sqrt(variance) / mean) * 100.0 : 0.0;

    BenchResult r;
    r.name = name;
    r.iterations = iterations;
    r.samples = samples;
    r.median_ns = median;
    r.min_ns = min_ns;
    r.max_ns = max_ns;
    r.cv_percent = cv;
    r.ops_per_sec = (median > 0.0) ? (1.0e9 / median) : 0.0;
    return r;
}

// ---------------------------------------------------------------------------
// Scheduler helpers
// ---------------------------------------------------------------------------
void scheduler_reset_two_tasks(TaskPriority prio_a, TaskPriority prio_b) {
    Scheduler::instance().init();
    (void)Scheduler::instance().create_task(bench_task_entry, g_stack0, sizeof(g_stack0), prio_a);
    (void)Scheduler::instance().create_task(bench_task_entry, g_stack1, sizeof(g_stack1), prio_b);
    Scheduler::instance().set_started(true);
    g_current_tcb_ptr = Scheduler::instance().get_task(0);
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
struct Options {
    uint64_t iterations = 200000;
    uint32_t samples = 9;
    std::string csv_path;
};

Options parse_options(int argc, char** argv);

void print_header(const Options& opt) {
    std::printf("AuroraOS host microbenchmark\n");
    std::printf("  iterations/sample : %llu\n", static_cast<unsigned long long>(opt.iterations));
    std::printf("  samples           : %u\n", opt.samples);
    std::printf("  warmup            : %llu iterations per benchmark (discarded)\n",
                static_cast<unsigned long long>(opt.iterations / 2 + 1));
    std::printf("  timing            : std::chrono::steady_clock, median of per-sample ns/op\n");
    std::printf("  compiler          : %s\n", AURORA_COMPILER_NAME);
    std::printf("  optimization      : %s\n\n",
#ifdef __OPTIMIZE__
                "enabled"
#else
                "disabled (numbers will be pessimistic)"
#endif
    );
}

void print_table(const std::vector<BenchResult>& results) {
    std::printf("%-30s %10s %10s %8s %14s\n", "benchmark", "ns/op", "min ns", "CV %", "ops/sec");
    std::printf("%-30s %10s %10s %8s %14s\n", "-----------------------------", "----------", "----------",
                "--------", "--------------");
    for (const BenchResult& r : results) {
        std::printf("%-30s %10.2f %10.2f %8.2f %14.0f\n", r.name.c_str(), r.median_ns, r.min_ns, r.cv_percent,
                    r.ops_per_sec);
    }
    std::printf("\nNote: relative numbers only; shared CI runners introduce noise.\n");
}

void write_csv(const std::vector<BenchResult>& results, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "warning: cannot write CSV file '%s'\n", path.c_str());
        return;
    }
    std::fprintf(f,
                 "benchmark,iterations_per_sample,samples,median_ns_per_op,min_ns_per_op,max_ns_per_op,cv_percent,"
                 "ops_per_sec\n");
    for (const BenchResult& r : results) {
        std::fprintf(f, "%s,%llu,%u,%.4f,%.4f,%.4f,%.4f,%.2f\n", r.name.c_str(),
                     static_cast<unsigned long long>(r.iterations), r.samples, r.median_ns, r.min_ns, r.max_ns,
                     r.cv_percent, r.ops_per_sec);
    }
    std::fclose(f);
}

Options parse_options(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--iterations") == 0 && i + 1 < argc) {
            opt.iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(arg, "--samples") == 0 && i + 1 < argc) {
            opt.samples = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(arg, "--csv") == 0 && i + 1 < argc) {
            opt.csv_path = argv[++i];
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            std::printf("usage: %s [--iterations N] [--samples N] [--csv PATH]\n", argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown or incomplete option: %s\n", arg);
            std::exit(2);
        }
    }
    if (opt.iterations == 0) {
        std::fprintf(stderr, "--iterations must be > 0\n");
        std::exit(2);
    }
    if (opt.samples == 0) {
        std::fprintf(stderr, "--samples must be > 0\n");
        std::exit(2);
    }
    return opt;
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse_options(argc, argv);
    print_header(opt);

    std::vector<BenchResult> results;

    // 1) Ready-queue remove + push: the bookkeeping every wakeup/sleep pays.
    {
        Scheduler::instance().init();
        (void)Scheduler::instance().create_task(bench_task_entry, g_stack0, sizeof(g_stack0), TaskPriority::Normal);

        results.push_back(run_benchmark(
            "sched_ready_remove+push", opt.iterations, opt.samples, []() -> uint64_t {
                Scheduler::instance().remove_ready(0);
                Scheduler::instance().push_ready(0);
                return 1;
            }));
    }

    // 2) Full schedule() with two equal-priority tasks: every call rotates the
    //    time slice and selects the other task, i.e. the real dispatch path
    //    (signal dispatch, bitmap selection, bookkeeping, switch request).
    {
        scheduler_reset_two_tasks(TaskPriority::Normal, TaskPriority::Normal);

        results.push_back(run_benchmark(
            "sched_schedule_2task", opt.iterations, opt.samples, []() -> uint64_t {
                Scheduler::instance().schedule();
                return Scheduler::instance().get_total_switches();
            }));
    }

    // 3) IPC send + receive fast path: receiver blocks in receive(), sender
    //    delivers with nb_call(), no reply is awaited.
    {
        Scheduler::instance().init();
        TaskControlBlock* receiver =
            Scheduler::instance().create_task(bench_task_entry, g_stack0, sizeof(g_stack0), TaskPriority::Normal);
        TaskControlBlock* sender =
            Scheduler::instance().create_task(bench_task_entry, g_stack1, sizeof(g_stack1), TaskPriority::Normal);
        Scheduler::instance().set_started(true);
        g_current_tcb_ptr = receiver;

        auroraos::kernel::Endpoint endpoint;
        alignas(8) uint32_t message[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        alignas(8) uint32_t message_out[8] = {};

        results.push_back(run_benchmark(
            "ipc_send+receive", opt.iterations, opt.samples,
            [&]() -> uint64_t {
                const auto recv_status =
                    endpoint.receive(receiver, message_out, sizeof(message_out),
                                     auroraos::kernel::IPC_TIMEOUT_INFINITE);
                const auto send_status = endpoint.nb_call(sender, message, sizeof(message), nullptr, 0);
                return static_cast<uint64_t>(static_cast<int64_t>(recv_status) + 3) +
                       static_cast<uint64_t>(static_cast<int64_t>(send_status) + 3);
            }));
    }

    // 4) Full IPC roundtrip: blocking call() + reply(), including the PIP
    //    recalculation that reply() performs after every wakeup.
    {
        Scheduler::instance().init();
        TaskControlBlock* receiver =
            Scheduler::instance().create_task(bench_task_entry, g_stack0, sizeof(g_stack0), TaskPriority::Normal);
        TaskControlBlock* sender =
            Scheduler::instance().create_task(bench_task_entry, g_stack1, sizeof(g_stack1), TaskPriority::Normal);
        Scheduler::instance().set_started(true);
        g_current_tcb_ptr = receiver;

        auroraos::kernel::Endpoint endpoint;
        alignas(8) uint32_t message[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        alignas(8) uint32_t message_out[8] = {};
        alignas(8) uint32_t reply_out[8] = {};

        results.push_back(run_benchmark(
            "ipc_call+receive+reply", opt.iterations, opt.samples,
            [&]() -> uint64_t {
                const auto recv_status =
                    endpoint.receive(receiver, message_out, sizeof(message_out),
                                     auroraos::kernel::IPC_TIMEOUT_INFINITE);
                const auto call_status = endpoint.call(sender, message, sizeof(message), reply_out, sizeof(reply_out),
                                                       auroraos::kernel::IPC_TIMEOUT_INFINITE);
                const auto reply_status = endpoint.reply(receiver, sender->scheduler.id, message, sizeof(message));
                return static_cast<uint64_t>(static_cast<int64_t>(recv_status) + 3) +
                       static_cast<uint64_t>(static_cast<int64_t>(call_status) + 3) +
                       static_cast<uint64_t>(static_cast<int64_t>(reply_status) + 3);
            }));
    }

    // 5) Capability lookup: the authorization step behind every syscall.
    {
        Scheduler::instance().init();
        TaskControlBlock* owner =
            Scheduler::instance().create_task(bench_task_entry, g_stack0, sizeof(g_stack0), TaskPriority::Normal);

        auroraos::kernel::KernelObject object(auroraos::kernel::ObjectType::MemoryRegion);
        auroraos::kernel::Capability cap{};
        cap.type = auroraos::kernel::CapType::Memory;
        cap.rights.read = true;
        cap.rights.write = true;
        cap.object = &object;
        (void)auroraos::kernel::CSpace::cap_insert(owner, 0, cap);

        results.push_back(run_benchmark(
            "cspace_lookup", opt.iterations, opt.samples, [&]() -> uint64_t {
                const auroraos::kernel::Capability* found = auroraos::kernel::CSpace::cap_lookup(owner, 0);
                return found ? static_cast<uint64_t>(found->type) : 0;
            }));
    }

    // 6) Capability insert + delete: retain/release plus bitmap maintenance.
    {
        Scheduler::instance().init();
        TaskControlBlock* owner =
            Scheduler::instance().create_task(bench_task_entry, g_stack0, sizeof(g_stack0), TaskPriority::Normal);

        auroraos::kernel::KernelObject object(auroraos::kernel::ObjectType::MemoryRegion);
        auroraos::kernel::Capability cap{};
        cap.type = auroraos::kernel::CapType::Memory;
        cap.rights.read = true;
        cap.object = &object;

        results.push_back(run_benchmark(
            "cspace_insert+delete", opt.iterations, opt.samples,
            [&]() -> uint64_t {
                const bool inserted = auroraos::kernel::CSpace::cap_insert(owner, 3, cap);
                const bool deleted = auroraos::kernel::CSpace::cap_delete(owner, 3);
                return (inserted ? 1u : 0u) + (deleted ? 2u : 0u);
            }));
    }

    // 7) TLSF heap: 64-byte allocate followed by immediate deallocate (split +
    //    coalesce path).
    {
        KernelHeap::instance().init(g_heap_region, g_heap_region + sizeof(g_heap_region));

        results.push_back(run_benchmark(
            "heap_alloc+free_64b", opt.iterations, opt.samples, []() -> uint64_t {
                void* ptr = KernelHeap::instance().allocate(64);
                const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
                KernelHeap::instance().deallocate(ptr);
                return address;
            }));
    }

    print_table(results);

    if (!opt.csv_path.empty()) {
        write_csv(results, opt.csv_path);
        std::printf("CSV written to %s\n", opt.csv_path.c_str());
    }

    return 0;
}
