#ifndef STDLIB_H
#define STDLIB_H

#if defined(AURORA_HOST_TEST) && defined(__has_include_next)
#if __has_include_next(<stdlib.h>)
#include_next <stdlib.h>
#if defined(__linux__) || defined(__unix__)
#include <alloca.h>
#endif
#endif
#endif

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#ifndef AURORA_HOST_TEST
#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void free(void* ptr);
void* realloc(void* ptr, size_t size);
void abort(void);
void exit(int status);
int abs(int x);
int atoi(const char* str);
float strtof(const char* nptr, char** endptr);

#ifdef __cplusplus
}
#endif
#endif

#endif
