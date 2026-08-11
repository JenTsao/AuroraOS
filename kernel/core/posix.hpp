#ifndef AURORA_POSIX_HPP
#define AURORA_POSIX_HPP

#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

// 线程本地 errno 宏
int* __errno_location();
#undef errno
#define errno (*__errno_location())


#ifdef __cplusplus
} // 关闭上面的 extern "C"
#endif

#ifdef AURORA_HOST_TEST
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AURORA_HOST_TEST
// 标准 POSIX 文件打开标志 — 仅在 newlib 的 <fcntl.h> 未包含时定义，避免重定义
#ifndef _SYS_FCNTL_H_
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0200
#define O_TRUNC     0x0400
#define O_APPEND    0x0800
#endif

// 包含 newlib 的 <fcntl.h> + <unistd.h> 获取 POSIX 函数声明。
// <fcntl.h> 提供 open()，<unistd.h> 提供 close/read/write/lseek/sleep/usleep。
// 项目在 posix.cpp 中提供基于 VFS 的实现，通过链接覆盖 newlib 的桩函数。
#ifndef _SYS_FCNTL_H_
#include <fcntl.h>
#endif
#ifndef _SYS_UNISTD_H
#include <unistd.h>
#endif

// ioctl 不在 newlib 裸机声明中，由项目提供
int ioctl(int fd, int request, void* arg);
#endif

// POSIX 信号量不透明指针
typedef void* sem_t;

// 信号量接口
int sem_init(sem_t* sem, int pshared, unsigned int value);
int sem_wait(sem_t* sem);
int sem_post(sem_t* sem);
int sem_destroy(sem_t* sem);

#ifdef __cplusplus
}
#endif

#endif
