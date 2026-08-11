#ifndef KERNEL_SYMBOL_EXPORT_HPP
#define KERNEL_SYMBOL_EXPORT_HPP

#include <stdint.h>

struct KernelSymbol {
    const char* name;
    uintptr_t addr;
};

extern const KernelSymbol kernel_symtab[];
extern const int kernel_symtab_size;

#endif // KERNEL_SYMBOL_EXPORT_HPP
