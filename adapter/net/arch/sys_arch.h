#ifndef AURORA_SYS_ARCH_H
#define AURORA_SYS_ARCH_H

// 1. 将 lwIP 的信号量映射为 void*
typedef void* sys_sem_t;

// 2. 将 lwIP 的互斥锁映射为 void*
typedef void* sys_mutex_t;

// 3. 将 lwIP 的消息队列映射为 void*
typedef void* sys_mbox_t;

// 4. 将 lwIP 的线程句柄映射为 void*
typedef void* sys_thread_t;

// 5. Lightweight protection (critical section state)
typedef int sys_prot_t;

// 6. 定义无效句柄常量
#define SYS_SEM_NULL (0)
#define SYS_MUTEX_NULL (0)
#define SYS_MBOX_NULL (0)

#endif // AURORA_SYS_ARCH_H
