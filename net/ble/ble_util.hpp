// ============================================================
// ble_util.hpp — small shared utilities for BLE security modules
//
// Header-only inline helpers used by gatt_auditor.hpp, ble_mitm.hpp
// and related scanners. Kept here so multiple header-only modules
// can share the same definition without link-time duplication.
// ============================================================
#pragma once

#include <stddef.h>

// Copy a NUL-terminated C-string into a fixed buffer, truncating to fit.
// The destination is always NUL-terminated. Truncation is silent because all
// current callers supply descriptions shorter than the buffer.
inline void copy_desc_bounded(char* dst, const char* src, int max_len) noexcept {
    int i = 0;
    while (i < max_len && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}
