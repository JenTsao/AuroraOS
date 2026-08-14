/*
 * Newlib system call stubs for bare-metal builds.
 * These are required by the C library (libg_nano) on ARM targets.
 * They delegate to the auroraOS POSIX layer where available.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdint.h>

// Bare-metal errno: avoid picolibc's TLS __thread errno which conflicts with
// lwIP's plain global errno (LWIP_PROVIDE_ERRNO=1).
#ifndef ENOMEM
#define ENOMEM 12
#endif
int errno = 0;

/* Forward declarations from posix.hpp */
extern int open(const char* path, int flags);
extern int close(int fd);
extern int read(int fd, char* buf, int len);
extern int write(int fd, const char* buf, int len);
extern int lseek(int fd, int offset, int whence);

int _close(int fd) {
    return close(fd);
}

int _fstat(int fd, struct stat* st) {
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

int _gettimeofday(struct timeval* tv, void* tz) {
    (void)tv;
    (void)tz;
    return 0;
}

int _isatty(int fd) {
    (void)fd;
    return 1; /* Assume all fds are terminals */
}

_off_t _lseek(int fd, _off_t offset, int whence) {
    return (_off_t)lseek(fd, (int)offset, whence);
}

int _read(int fd, char* buf, int len) {
    return read(fd, buf, len);
}

int _write(int fd, const char* buf, int len) {
    return write(fd, buf, len);
}

extern uint32_t _heap_start;
extern uint32_t _heap_end;
static char* _heap_ptr = 0;

void* _sbrk(int incr) {
    if (_heap_ptr == 0) {
        _heap_ptr = (char*)&_heap_start;
    }
    char* prev = _heap_ptr;
    // 用整数地址比较，避免比较指向不同对象的指针（C 标准未定义行为，
    // cppcheck 会报 comparePointers）。
    char* new_ptr = _heap_ptr + incr;
    if ((uintptr_t)new_ptr > (uintptr_t)&_heap_end) {
        errno = ENOMEM;
        return (void*)-1;
    }
    _heap_ptr += incr;
    return prev;
}

void _exit(int status) {
    (void)status;
    while (1) {}
}
