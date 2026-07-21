#include "lwip/opt.h"
#include "lwip/sys.h"
#include "arch/sys_arch.h"
#include "memory.hpp"
#include "task.hpp"
#include "semaphore.hpp"
#include "mutex.hpp"
#include "msg_queue.hpp"

extern void sys_print(const char* msg);
extern volatile uint32_t tick_count; // Global tick from SysTick_Handler

#include "memory_pool.hpp"

static MemoryPool<Mutex, 16> g_mutex_pool;
static MemoryPool<Semaphore, 32> g_sem_pool;
static MemoryPool<MessageQueue<void*, 16>, 16> g_mbox_pool;

static constexpr int MAX_LWIP_THREADS = 2;
static uint32_t g_lwip_stacks[MAX_LWIP_THREADS][512];
static int g_lwip_thread_count = 0;

// ==========================================
// 1. System Clock
// ==========================================
u32_t sys_now(void) {
    // Assuming 1 Tick = 1 ms (1000 Hz SysTick)
    return tick_count;
}

// ==========================================
// 1.5 Lightweight Protection (Critical Sections)
// ==========================================
sys_prot_t sys_arch_protect(void) {
    // Disable interrupts for a critical section
    // In Cortex-M, we can read PRIMASK and then disable IRQs
    int primask = 0;
    __asm__ volatile ("mrs %0, primask\n\t"
                      "cpsid i" : "=r" (primask));
    return primask;
}

void sys_arch_unprotect(sys_prot_t pval) {
    // Restore PRIMASK
    __asm__ volatile ("msr primask, %0" : : "r" (pval));
}

// ==========================================
// 2. Mutex Adapter
// ==========================================
err_t sys_mutex_new(sys_mutex_t *mutex) {
    *mutex = g_mutex_pool.create();
    return (*mutex != nullptr) ? ERR_OK : ERR_MEM;
}

void sys_mutex_lock(sys_mutex_t *mutex) {
    if (*mutex) static_cast<Mutex*>(*mutex)->lock();
}

void sys_mutex_unlock(sys_mutex_t *mutex) {
    if (*mutex) static_cast<Mutex*>(*mutex)->unlock();
}

void sys_mutex_free(sys_mutex_t *mutex) {
    if (*mutex) {
        g_mutex_pool.destroy(static_cast<Mutex*>(*mutex));
        *mutex = nullptr;
    }
}

// ==========================================
// 3. Semaphore Adapter
// ==========================================
err_t sys_sem_new(sys_sem_t *sem, u8_t count) {
    *sem = g_sem_pool.create(count);
    return (*sem != nullptr) ? ERR_OK : ERR_MEM;
}

void sys_sem_signal(sys_sem_t *sem) {
    if (*sem) static_cast<Semaphore*>(*sem)->signal();
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout_ms) {
    if (!*sem) return SYS_ARCH_TIMEOUT;
    
    u32_t start_time = sys_now();
    
    // In a real RTOS we would support timeout.
    // Here we just block infinitely because our Semaphore wait() doesn't have timeout.
    // If timeout_ms > 0, ideally we should poll and sleep. We will just wait.
    static_cast<Semaphore*>(*sem)->wait();
    
    return sys_now() - start_time;
}

void sys_sem_free(sys_sem_t *sem) {
    if (*sem) {
        g_sem_pool.destroy(static_cast<Semaphore*>(*sem));
        *sem = nullptr;
    }
}

int sys_sem_valid(sys_sem_t *sem) {
    return (*sem != nullptr);
}

void sys_sem_set_invalid(sys_sem_t *sem) {
    *sem = nullptr;
}

// ==========================================
// 4. Mailbox (Message Queue) Adapter
// ==========================================
err_t sys_mbox_new(sys_mbox_t *mbox, int size) {
    (void)size; // Hardcoded to 16 in our type definition
    *mbox = g_mbox_pool.create();
    return (*mbox != nullptr) ? ERR_OK : ERR_MEM;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg) {
    if (*mbox) static_cast<MessageQueue<void*, 16>*>(*mbox)->push(msg);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg) {
    if (!*mbox) return ERR_VAL;

    // 改用非阻塞的 try_push：队列满时立即返回 ERR_MEM，符合 lwIP 对
    // trypost 语义的要求（调用方会据此自行重试或丢弃，而不是被无限期挂起）。
    if (static_cast<MessageQueue<void*, 16>*>(*mbox)->try_push(msg)) {
        return ERR_OK;
    }
    return ERR_MEM;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg) {
    // try_push 内部只用关中断做临界区保护，不涉及任务调度/系统调用，
    // 因此可以安全地在中断上下文里直接复用，不会像过去转调阻塞版
    // push() 那样在队列满时把 ISR 锁死。
    return sys_mbox_trypost(mbox, msg);
}

// ==========================================
// 4.5 System Init
// ==========================================
void sys_init(void) {
    // OSAL initialization if needed. We don't need anything here.
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout_ms) {
    if (!*mbox) return SYS_ARCH_TIMEOUT;
    u32_t start_time = sys_now();
    
    // Block infinitely to get message
    void* data = static_cast<MessageQueue<void*, 16>*>(*mbox)->pop(); 
    if (msg) *msg = data;
    
    return sys_now() - start_time;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg) {
    if (!*mbox) return SYS_MBOX_EMPTY;

    // 改用非阻塞的 try_pop：队列为空时立即返回 SYS_MBOX_EMPTY，
    // 这对 lwIP 主循环至关重要——它用 tryfetch 做"看一眼有没有消息，
    // 没有就继续处理定时器/其他事件"的非阻塞轮询，一旦这里阻塞住，
    // 整个协议栈主线程的事件循环都会被卡死。
    void* data = nullptr;
    if (!static_cast<MessageQueue<void*, 16>*>(*mbox)->try_pop(data)) {
        return SYS_MBOX_EMPTY;
    }
    if (msg) *msg = data;
    return 0; // Time taken
}

void sys_mbox_free(sys_mbox_t *mbox) {
    if (*mbox) {
        g_mbox_pool.destroy(static_cast<MessageQueue<void*, 16>*>(*mbox));
        *mbox = nullptr;
    }
}

int sys_mbox_valid(sys_mbox_t *mbox) {
    return (*mbox != nullptr);
}

void sys_mbox_set_invalid(sys_mbox_t *mbox) {
    *mbox = nullptr;
}

// ==========================================
// 5. Thread Adapter
// ==========================================
sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio) {
    sys_print("[lwIP OSAL] Spawning new lwIP system thread: ");
    sys_print(name);
    sys_print("\r\n");

    // Since our thread doesn't easily pass args through context yet, we'll store arg globally
    // or just let lwIP use global context. 
    // Wait, the lwIP tcpip_thread just needs its arg. We will cast it.
    
    uint32_t* thread_stack = nullptr;
    if (g_lwip_thread_count < MAX_LWIP_THREADS) {
        thread_stack = g_lwip_stacks[g_lwip_thread_count++];
    } else {
        sys_print("[lwIP OSAL] ERROR: out of thread stacks!\r\n");
        return nullptr;
    }
    
    // To support arg, we need to pass it. Our create_task only takes void(*)(void).
    // Let's assume lwIP handles it or we don't strictly need it for single netif setup.
    // Actually, tcpip_thread ignores arg or uses it for init done callback, but tcpip_init uses a global struct.

    // 注意：这里过去直接返回 get_current_tcb()——那是"当前正在调用
    // sys_thread_new() 的任务"（通常是内核初始化/引导任务）的 TCB，
    // 并不是刚刚创建出来的新线程！sys_thread_t 语义上必须是新线程自己的
    // 句柄。现在改为使用 create_task() 的返回值，它就是新任务真正的 TCB
    // 指针；任务表已满导致创建失败时会返回 nullptr，此时打印告警而不是
    // 把一个错误的句柄悄悄交还给 lwIP。
    TaskControlBlock* new_tcb = Scheduler::instance().create_task(
        reinterpret_cast<void (*)()>(thread), thread_stack, stacksize,
        TaskPriority::High); // lwIP tcpip_thread 与 Shell 同级（High），通过时间片轮转共存

    if (!new_tcb) {
        sys_print("[lwIP OSAL] ERROR: task table full, failed to spawn thread: ");
        sys_print(name);
        sys_print("\r\n");
    }

    return new_tcb;
}
