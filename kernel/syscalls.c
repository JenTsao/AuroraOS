/*
 * Newlib system call stubs for bare-metal builds.
 * These are required by the C library (libg_nano) on ARM targets.
 * They delegate to the auroraOS POSIX layer where available.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

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
    return 1;  /* Assume all fds are terminals */
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

extern char* _heap_start;
extern char* _heap_end;
static char* _heap_ptr = 0;

void* _sbrk(int incr) {
    if (_heap_ptr == 0) {
        _heap_ptr = _heap_start;
    }
    char* prev = _heap_ptr;
    if (_heap_ptr + incr > _heap_end) {
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
