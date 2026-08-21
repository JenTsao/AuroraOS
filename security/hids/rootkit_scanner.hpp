// =============================================================================
// security/hids/rootkit_scanner.hpp
//
// HIDS 内核级 Rootkit 扫描
//
//   - 内核堆完整性：沿 TLSF 物理块链表校验魔数（KernelHeap::verify_integrity）
//   - 函数序言完整性：校验关键内核函数首字节，检测钩子/补丁篡改（check_prologue）
//
// 复用 AuroraOS 特性：TLSF 魔数校验、SecurityMonitor（经引擎上报）
// 设计原则（遵循 AGENTS.md）：零堆分配、noexcept
// =============================================================================
#ifndef AURORA_HIDS_ROOTKIT_SCANNER_HPP
#define AURORA_HIDS_ROOTKIT_SCANNER_HPP

#include <stdint.h>
#include "../../kernel/mm/memory.hpp"

namespace aurora {
namespace hids {

class RootkitScanner {
public:
    // 扫描内核完整性，返回本次新发现数
    int scan() {
        int findings = 0;

        // 1. 内核堆完整性（TLSF 魔数）
        const size_t corrupt = KernelHeap::instance().verify_integrity();
        if (corrupt > last_heap_corrupt_) {
            findings += static_cast<int>(corrupt - last_heap_corrupt_);
            record_("kernel heap corruption");
        }
        last_heap_corrupt_ = corrupt;

        return findings;
    }

    // 函数序言完整性校验：首 n 字节与期望值不符则视为被篡改。
    // 返回 true 表示一致，false 表示被篡改。
    static bool check_prologue(const void* func, const uint8_t* expected, int n) noexcept {
        if (!func || !expected || n <= 0)
            return false;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(func);
        for (int i = 0; i < n; ++i) {
            if (p[i] != expected[i])
                return false;
        }
        return true;
    }

    const char* get_name() const noexcept {
        return "rootkit_scanner";
    }

    uint32_t get_total_findings() const noexcept {
        return total_findings_;
    }

    size_t get_heap_corrupt_count() const noexcept {
        return last_heap_corrupt_;
    }

    const char* get_last_finding() const noexcept {
        return last_finding_;
    }

    void reset() noexcept {
        total_findings_ = 0;
        last_heap_corrupt_ = 0;
        last_finding_[0] = '\0';
    }

private:
    uint32_t total_findings_ = 0;
    size_t last_heap_corrupt_ = 0;
    char last_finding_[64]{};

    void record_(const char* what) {
        ++total_findings_;
        char* p = last_finding_;
        const char* const end = last_finding_ + sizeof(last_finding_) - 1;
        while (*what && p < end)
            *p++ = *what++;
        *p = '\0';
    }
};

} // namespace hids
} // namespace aurora

#endif // AURORA_HIDS_ROOTKIT_SCANNER_HPP
