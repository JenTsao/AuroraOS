#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void free(void* ptr);
void* realloc(void* ptr, size_t size);
void abort(void);
void exit(int status);
int abs(int x);
float strtof(const char* nptr, char** endptr);

#ifdef __cplusplus
}
#endif

#endif
