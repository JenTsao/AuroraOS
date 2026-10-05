#include "kernel_selftest.hpp"

#include "syscall.hpp"
#include "../kernel/task/task.hpp"
#include "../kernel/core/ipc.hpp"

extern "C" void uart_puts(const char* s);

#ifdef CONFIG_WATCHDOG
#include "../kernel/core/watchdog_manager.hpp"
#endif

// =============================================================================
// Design notes
//   * Every probe task uses a static stack: no heap, no lifetime surprises.
//   * Workers never return (they sleep forever once done) because the entry
//     point has no defined return path; the suite terminates them instead.
//   * Priorities are chosen relative to the shell (TaskPriority::High = 16):
//       - Low (4)  workers only run while the shell is asleep
//       - Net_TX (18) workers pre-empt the shell
//   * All output for the *report* goes through uart_puts so the report cannot
//     be lost to syscall validation.  sys_print()/sys_get_time() are still
//     exercised on purpose as their own test cases.
// =============================================================================

namespace {

// Probe task stack sizing: the guest trap entry pushes a ~132 byte frame and
// the C trap handler runs on top of it, so a probe task needs real headroom.
// 160 words was enough for Cortex-M3 but overflowed on RISC-V (the fault showed
// up as an instruction access fault inside the trap epilogue).
constexpr uint32_t ST_STACK_WORDS = 512; // 2 KiB per probe task

// ── probe task state (create_task takes void(*)(void), so no captures) ──────
volatile uint32_t g_rr0_count = 0;
volatile uint32_t g_rr0_done = 0;
volatile uint32_t g_rr1_count = 0;
volatile uint32_t g_rr1_done = 0;
volatile uint32_t g_hi_ran = 0;

uint32_t g_stack_a[ST_STACK_WORDS];
uint32_t g_stack_b[ST_STACK_WORDS];
uint32_t g_stack_c[ST_STACK_WORDS];
uint32_t g_stack_d[ST_STACK_WORDS];
uint32_t g_stack_e[ST_STACK_WORDS];

TaskControlBlock* g_created[8];
uint32_t g_created_n = 0;

uint32_t g_pass = 0;
uint32_t g_total = 0;

// ── reporting ───────────────────────────────────────────────────────────────
void st_put_u32(uint32_t v) {
    char buf[12];
    int i = 0;
    if (v == 0)
        buf[i++] = '0';
    while (v > 0) {
        buf[i++] = static_cast<char>('0' + (v % 10u));
        v /= 10u;
    }
    for (int j = 0; j < i / 2; ++j) {
        const char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
    buf[i] = '\0';
    uart_puts(buf);
}

void report(const char* name, bool pass) {
    char line[64];
    int i = 0;
    const char* p = "[SELFTEST] ";
    while (*p && i < 40)
        line[i++] = *p++;
    p = name;
    while (*p && i < 52)
        line[i++] = *p++;
    if (i < 58) {
        line[i++] = ':';
        line[i++] = ' ';
    }
    p = pass ? "PASS\r\n" : "FAIL\r\n";
    while (*p && i < 62)
        line[i++] = *p++;
    line[i] = '\0';
    uart_puts(line);

    g_total++;
    if (pass)
        g_pass++;
}

// ── probe task bodies ───────────────────────────────────────────────────────
// Yields 25 times through the REAL svc/ecall entry so the scheduler has to
// switch away and back on every iteration, then parks itself.
void rr_worker(volatile uint32_t* count, volatile uint32_t* done) {
    for (uint32_t i = 0; i < 25; ++i) {
        (*count)++;
        sys_yield();
    }
    *done = 1;
    for (;;) {
        sys_sleep(1000);
    }
}

void rr_worker0() {
    rr_worker(&g_rr0_count, &g_rr0_done);
}

void rr_worker1() {
    rr_worker(&g_rr1_count, &g_rr1_done);
}

void hi_worker() {
    g_hi_ran = 1;
    for (;;) {
        sys_sleep(1000);
    }
}

void sleeper_worker() {
    for (;;) {
        sys_sleep(1000);
    }
}

TaskControlBlock* spawn(void (*entry)(void), uint32_t* stack, TaskPriority prio) {
    TaskControlBlock* t = Scheduler::instance().create_task(entry, stack, ST_STACK_WORDS * sizeof(uint32_t), prio);
    if (t && g_created_n < 8)
        g_created[g_created_n++] = t;
    return t;
}

void cleanup() {
    for (uint32_t i = 0; i < g_created_n; ++i) {
        TaskControlBlock* t = g_created[i];
        if (!t)
            continue;
        if (t->scheduler.state == TaskState::Terminated || t->scheduler.state == TaskState::Unallocated)
            continue;
        Scheduler::instance().terminate_task(t->scheduler.id);
    }
    g_created_n = 0;
}

// ── case 1: two same-priority tasks must genuinely round-robin ─────────────
// Exercises: create_task -> push_ready -> SysTick -> schedule() -> context
// switch -> switch back, plus the switch counter accounting.
bool case_ctx_switch_roundrobin() {
    g_rr0_count = 0;
    g_rr0_done = 0;
    g_rr1_count = 0;
    g_rr1_done = 0;

    const uint32_t switches_before = Scheduler::instance().get_total_switches();

    TaskControlBlock* a = spawn(rr_worker0, g_stack_a, TaskPriority::Low);
    TaskControlBlock* b = spawn(rr_worker1, g_stack_b, TaskPriority::Low);
    if (!a || !b)
        return false;

    // The shell sits at High (16); sleeping releases the CPU so the two Low
    // (4) workers can alternate on the real context-switch path.
    Scheduler::instance().sleep_ms(60);

    const uint32_t switches_after = Scheduler::instance().get_total_switches();
    return g_rr0_done == 1 && g_rr1_done == 1 && g_rr0_count == 25 && g_rr1_count == 25 &&
           switches_after > switches_before;
}

// ── case 2: a higher-priority task must pre-empt the shell ─────────────────
bool case_priority_preempt() {
    g_hi_ran = 0;
    TaskControlBlock* t = spawn(hi_worker, g_stack_c, TaskPriority::Net_TX); // 18 > High (16)
    if (!t)
        return false;

    Scheduler::instance().sleep_ms(20);
    return g_hi_ran == 1;
}

// ── case 3: a smashed stack canary must be detected and the task killed ────
// tick_update() runs the watermark check on every SysTick; this is the guard
// that keeps a overflowing task from corrupting kernel data.
bool case_stack_canary_detect() {
    TaskControlBlock* t = spawn(sleeper_worker, g_stack_d, TaskPriority::Low);
    if (!t || t->stack_canary_ptr == nullptr)
        return false;
    if (*t->stack_canary_ptr != Scheduler::STACK_CANARY)
        return false;

    *t->stack_canary_ptr = 0xBAD0BAD0u; // simulate an overflow stomp
    Scheduler::instance().sleep_ms(40); // let tick_update() notice

    return t->scheduler.state == TaskState::Terminated;
}

// ── case 4: syscall entry must carry a return value back through the trap ──
// sys_get_time() performs svc/ecall; the dispatcher writes the answer into
// the interrupt frame and the trap return puts it in the caller's register.
bool case_svc_roundtrip() {
    const uint32_t t0 = sys_get_time();
    Scheduler::instance().sleep_ms(30);
    const uint32_t t1 = sys_get_time();
    return t1 > t0; // strictly increasing proves a live clock came back
}

// ── case 5: the print syscall itself (visibility double-checked by CI) ─────
bool case_svc_print_path() {
    // If this line shows up on the serial console, the SVC entry, the
    // dispatcher's pointer validation and the UART driver all worked.
    sys_print("[SELFTEST] svc_print: delivered through the real SVC entry\r\n");
    return true;
}

// ── case 6: a blocking IPC receive must come back with its real result ─────
// 接收方在 sys_ipc_receive 里挂起时，内核还拿不到结果（PendSV 要等这次 SVC 退出
// 才执行），所以返回值只能事后投递。修复前接收方从 SVC 返回的永远是中间态
// Blocked(1)，out_sender_id 也是上一次等待的残值，于是它没法 reply。
volatile int g_ipc_ret = -999;
volatile uint32_t g_ipc_sender = 0xDEADu;
volatile uint32_t g_ipc_ran = 0;
char g_ipc_msgbuf[32];

// 命名空间作用域而非函数内 static：freestanding 下没有 __cxa_guard_acquire，
// 函数局部 static 会链接失败。
auroraos::kernel::Endpoint g_st_ep;

void ipc_receiver_worker() {
    uint32_t sender = 0xDEADu;
    const int r = sys_ipc_receive(3, g_ipc_msgbuf, sizeof(g_ipc_msgbuf), &sender);
    g_ipc_ret = r;
    g_ipc_sender = sender;
    g_ipc_ran = 1;
    for (;;) {
        sys_sleep(1000);
    }
}

bool case_ipc_blocking_receive_returns_result() {
    g_ipc_ret = -999;
    g_ipc_sender = 0xDEADu;
    g_ipc_ran = 0;
    g_ipc_msgbuf[0] = '\0';

    TaskControlBlock* rx = spawn(ipc_receiver_worker, g_stack_e, TaskPriority::Low);
    if (!rx)
        return false;

    // 接收方需要一枚指向 g_st_ep 的读权能。此处直接写槽位（与 host 侧测试一致）；
    // 新任务优先级低于 shell，shell 睡眠前不会跑，所以不存在配置未就绪就被读取。
    rx->security.cspace[3].type = auroraos::kernel::CapType::Endpoint;
    rx->security.cspace[3].rights = {1, 0, 0, 0}; // read
    rx->security.cspace[3].badge = 0x5A;
    rx->security.cspace[3].object = &g_st_ep;

    Scheduler::instance().sleep_ms(30); // 让接收方跑起来并挂进端点等待队列
    if (g_ipc_ran != 0)
        return false; // 不该在收到消息前就返回
    if (rx->ipc.state != auroraos::kernel::IpcState::Receiving)
        return false;

    char msg[] = "selftest";
    TaskControlBlock* self = Scheduler::instance().get_current_tcb();
    if (!self)
        return false;
    const uint32_t self_id = self->scheduler.id;

    // 非阻塞投递：命中 fast-path，shell 自己不会被挂起
    const auroraos::kernel::IpcStatus st =
        g_st_ep.call(self, msg, sizeof(msg), nullptr, 0, auroraos::kernel::IPC_NONBLOCK, 0x5A);
    const bool ok = (st == auroraos::kernel::IpcStatus::Ok);
    self->ipc.state = auroraos::kernel::IpcState::Ready; // 不给自己留下 AwaitReply 残留

    Scheduler::instance().sleep_ms(30); // 让接收方醒来读走投递的结果
    if (!ok)
        return false;
    if (g_ipc_ran != 1)
        return false;
    if (g_ipc_ret != static_cast<int>(auroraos::kernel::IpcStatus::Ok))
        return false;
    if (g_ipc_sender != self_id)
        return false;

    const char* want = "selftest";
    for (int i = 0; i < 9; ++i) {
        if (g_ipc_msgbuf[i] != want[i])
            return false;
    }
    return true;
}

#if defined(CONFIG_WATCHDOG)
// ── case 7: the scheduler must keep the watchdog fed while multi-tasking ───
bool case_watchdog_fed() {    WatchdogDriver* wdt = WatchdogManager::instance().get_driver();
    if (!wdt)
        return false;

    // 40 ms of real scheduling; schedule() feeds the watchdog on every pass,
    // so the counter must never reach zero.
    Scheduler::instance().sleep_ms(40);
    return wdt->get_remaining() > 0;
}
#endif

} // namespace

void run_kernel_selftest() {
    g_pass = 0;
    g_total = 0;
    g_created_n = 0;

    uart_puts("\r\n[SELFTEST] === on-target kernel self-test (real scheduler / trap path) ===\r\n");

    report("svc_print_path", case_svc_print_path());
    report("ctx_switch_roundrobin", case_ctx_switch_roundrobin());
    report("priority_preempt", case_priority_preempt());
    report("svc_get_time_roundtrip", case_svc_roundtrip());
    report("stack_canary_detect", case_stack_canary_detect());
    report("ipc_blocking_receive_result", case_ipc_blocking_receive_returns_result());
#if defined(CONFIG_WATCHDOG)
    report("watchdog_fed", case_watchdog_fed());
#endif

    cleanup();

    const bool all_pass = (g_pass == g_total) && (g_total > 0);
    uart_puts("[SELFTEST] SUMMARY: ");
    st_put_u32(g_pass);
    uart_puts("/");
    st_put_u32(g_total);
    uart_puts(all_pass ? " PASS\r\n" : " FAIL\r\n");
    uart_puts(all_pass ? "[SELFTEST] RESULT: PASS\r\n" : "[SELFTEST] RESULT: FAIL\r\n");
}
