#include "posix.hpp"
#include "vfs.hpp"
#include "task.hpp"
#include "semaphore.hpp"

#include "syscall.hpp"

// POSIX 函数实现 — 签名与 newlib <unistd.h> 声明保持一致
// open() 为 variadic，lseek 用 off_t，usleep 用 useconds_t

extern "C" {

#ifndef AURORA_HOST_TEST
#include <stdarg.h>
#include <sys/types.h>  // off_t, useconds_t

int open(const char* path, int flags, ...) {
#ifdef CONFIG_VFS
    int res = VfsManager::instance().open(path, flags);
    if (res < 0) {
        errno = ENOENT;
        return -1;
    }
    return res;
#else
    errno = ENOSYS;
    return -1;
#endif
}

int close(int fd) {
#ifdef CONFIG_VFS
    if (VfsManager::instance().close(fd) < 0) {
        errno = EBADF;
        return -1;
    }
    return 0;
#else
    (void)fd;
    errno = ENOSYS;
    return -1;
#endif
}

int read(int fd, void* buf, size_t count) {
#ifdef CONFIG_VFS
    int res = VfsManager::instance().read(fd, static_cast<char*>(buf), count);
    if (res < 0) {
        errno = EIO;
        return -1;
    }
    return res;
#else
    (void)fd; (void)buf; (void)count;
    errno = ENOSYS;
    return -1;
#endif
}

int write(int fd, const void* buf, size_t count) {
#ifdef CONFIG_VFS
    int res = VfsManager::instance().write(fd, static_cast<const char*>(buf), count);
    if (res < 0) {
        errno = EIO;
        return -1;
    }
    return res;
#else
    (void)fd; (void)buf; (void)count;
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
    (void)fd; (void)request; (void)arg;
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
    (void)fd; (void)offset; (void)whence;
    errno = ENOSYS;
    return -1;
#endif
}

unsigned int sleep(unsigned int seconds) {
    // 假设 1 tick = 1ms，这里转换为 ticks 延时
    // 防止无符号整数溢出（UINT32_MAX / 1000 约等于 4294967）
    if (seconds > 4294967) seconds = 4294967;
    sys_sleep(seconds * 1000);
    return 0;
}

int usleep(useconds_t usec) {
    uint32_t ticks = usec / 1000;
    if (ticks == 0) ticks = 1; // 至少休眠 1 个 tick 让出 CPU
    sys_sleep(ticks);
    return 0;
}
#endif


int sem_init(sem_t* sem, int pshared, unsigned int value) {
    (void)pshared;
    Semaphore* s = new Semaphore(value);
    if (!s) {
        errno = ENOMEM;
        *sem = nullptr;
        return -1;
    }
    *sem = s;
    return 0;
}

int sem_wait(sem_t* sem) {
    Semaphore* s = static_cast<Semaphore*>(*sem);
    if (s) {
        s->wait();
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int sem_post(sem_t* sem) {
    Semaphore* s = static_cast<Semaphore*>(*sem);
    if (s) {
        s->signal();
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int sem_destroy(sem_t* sem) {
    Semaphore* s = static_cast<Semaphore*>(*sem);
    if (s) {
        delete s;
        *sem = nullptr;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

}
