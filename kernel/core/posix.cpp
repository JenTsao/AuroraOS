#include "posix.hpp"
#include "vfs.hpp"
#include "task.hpp"
#include "semaphore.hpp"
#include "audit.hpp"
#include "syscall_validator.hpp"

#include "syscall.hpp"

// POSIX 函数实现 — 签名与 newlib <unistd.h> 声明保持一致
// open() 为 variadic，lseek 用 off_t，usleep 用 useconds_t

extern "C" {

#ifndef AURORA_HOST_TEST
#include <stdarg.h>
#include <sys/types.h> // off_t, useconds_t

int open(const char* path, int flags, ...) {
    // 审计路径校验（DS-06）：审计钩子会无校验地读取 path[0..14]。
    // 用户态传入的恶意/错误指针会在内核态解引用任意地址 → BusFault/HardFault，
    // 或借 /proc/audit_log 泄漏内核内存。校验失败时向审计层传 nullptr
    // （record_path 已有空指针分支），路径拒绝后 errno 置 EFAULT。
    bool path_readable = true;
    {
        TaskControlBlock* audit_cur = Scheduler::instance().get_current_tcb();
        // 仅约束 User 特权任务的指针来源（Kernel 任务可访问内核数据段/堆，
        // 且 ota 等内核组件以内核字符串调用 open()）。
        if (audit_cur && audit_cur->privilege == static_cast<uint32_t>(TaskPrivilege::User) &&
            !auroraos::kernel::SyscallValidator::validate_user_ptr(path, 16, audit_cur, false)) {
            path_readable = false;
        }
    }
    const char* audit_path = path_readable ? path : nullptr;

#ifdef CONFIG_VFS
    int res = VfsManager::instance().open(path, flags);
    AUDIT_HOOK_OPEN(audit_path, res, flags);
    if (res < 0) {
        if (!path_readable) {
            errno = EFAULT;
        } else {
            errno = ENOENT;
        }
        return -1;
    }
    return res;
#else
    AUDIT_HOOK_OPEN(audit_path, -1, flags);
    if (!path_readable) {
        errno = EFAULT;
    } else {
        errno = ENOSYS;
    }
    return -1;
#endif
}

int close(int fd) {
#ifdef CONFIG_VFS
    int ret = VfsManager::instance().close(fd);
    AUDIT_HOOK_CLOSE(fd, ret);
    if (ret < 0) {
        errno = EBADF;
        return -1;
    }
    return 0;
#else
    (void)fd;
    AUDIT_HOOK_CLOSE(fd, -1);
    errno = ENOSYS;
    return -1;
#endif
}

ssize_t read(int fd, void* buf, size_t count) {
#ifdef CONFIG_VFS
    int res = VfsManager::instance().read(fd, static_cast<char*>(buf), count);
    AUDIT_HOOK_READ(fd, res);
    if (res < 0) {
        errno = EIO;
        return -1;
    }
    return res;
#else
    (void)fd;
    (void)buf;
    (void)count;
    AUDIT_HOOK_READ(fd, -1);
    errno = ENOSYS;
    return -1;
#endif
}

ssize_t write(int fd, const void* buf, size_t count) {
#ifdef CONFIG_VFS
    int res = VfsManager::instance().write(fd, static_cast<const char*>(buf), count);
    AUDIT_HOOK_WRITE(fd, res);
    if (res < 0) {
        errno = EIO;
        return -1;
    }
    return res;
#else
    (void)fd;
    (void)buf;
    (void)count;
    AUDIT_HOOK_WRITE(fd, -1);
    errno = ENOSYS;
    return -1;
#endif
}

int ioctl(int fd, int request, void* arg) {
#ifdef CONFIG_VFS
    int res = VfsManager::instance().ioctl(fd, request, arg);
    if (res < 0) {
        errno = EINVAL;
        return -1;
    }
    return res;
#else
    (void)fd;
    (void)request;
    (void)arg;
    errno = ENOSYS;
    return -1;
#endif
}

off_t lseek(int fd, off_t offset, int whence) {
#ifdef CONFIG_VFS
    int res = VfsManager::instance().lseek(fd, offset, whence);
    if (res < 0) {
        errno = EINVAL;
        return -1;
    }
    return res;
#else
    (void)fd;
    (void)offset;
    (void)whence;
    errno = ENOSYS;
    return -1;
#endif
}

unsigned int sleep(unsigned int seconds) {
    // 假设 1 tick = 1ms，这里转换为 ticks 延时
    // 防止无符号整数溢出（UINT32_MAX / 1000 约等于 4294967）
    if (seconds > 4294967)
        seconds = 4294967;
    sys_sleep(seconds * 1000);
    return 0;
}

int usleep(useconds_t usec) {
    uint32_t ticks = usec / 1000;
    if (ticks == 0)
        ticks = 1; // 至少休眠 1 个 tick 让出 CPU
    sys_sleep(ticks);
    return 0;
}
#endif

constexpr size_t MAX_POSIX_SEMAPHORES = 16;
struct PosixSemSlot {
    Semaphore sem;
    bool in_use = false;
};
static PosixSemSlot s_posix_sem_pool[MAX_POSIX_SEMAPHORES];

int sem_init(sem_t* sem, int pshared, unsigned int value) {
    (void)pshared;
    if (!sem) {
        errno = EINVAL;
        return -1;
    }

    IrqGuard guard;
    for (size_t i = 0; i < MAX_POSIX_SEMAPHORES; i++) {
        if (!s_posix_sem_pool[i].in_use) {
            s_posix_sem_pool[i].in_use = true;
            s_posix_sem_pool[i].sem.init(static_cast<int>(value));
            *sem = &s_posix_sem_pool[i].sem;
            return 0;
        }
    }
    errno = ENOSPC;
    *sem = nullptr;
    return -1;
}

int sem_wait(sem_t* sem) {
    if (!sem || !*sem) {
        errno = EINVAL;
        return -1;
    }
    Semaphore* s = static_cast<Semaphore*>(*sem);
    return s->wait() ? 0 : -1;
}

int sem_trywait(sem_t* sem) {
    if (!sem || !*sem) {
        errno = EINVAL;
        return -1;
    }
    Semaphore* s = static_cast<Semaphore*>(*sem);
    if (s->try_wait()) {
        return 0;
    }
    errno = EAGAIN;
    return -1;
}

int sem_post(sem_t* sem) {
    if (!sem || !*sem) {
        errno = EINVAL;
        return -1;
    }
    Semaphore* s = static_cast<Semaphore*>(*sem);
    s->signal();
    return 0;
}

int sem_destroy(sem_t* sem) {
    if (!sem || !*sem) {
        errno = EINVAL;
        return -1;
    }
    Semaphore* s = static_cast<Semaphore*>(*sem);
    IrqGuard guard;
    for (size_t i = 0; i < MAX_POSIX_SEMAPHORES; i++) {
        if (&s_posix_sem_pool[i].sem == s && s_posix_sem_pool[i].in_use) {
            s_posix_sem_pool[i].in_use = false;
            *sem = nullptr;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}
}
