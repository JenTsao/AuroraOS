#ifndef AURORA_POWER_WAKE_LOCK_REGISTRY_HPP
#define AURORA_POWER_WAKE_LOCK_REGISTRY_HPP

#include <stdint.h>

// ========================================================
// WakeLock 类型
// ========================================================
enum class WakeLockType : uint8_t {
    // 只阻止 CPU/调度进入深睡，允许屏幕按超时正常变暗、息屏。
    // 典型使用者：BLE 同步、OTA、日志落盘、后台算法。
    PARTIAL = 0,
    // 额外要求屏幕保持点亮（阻止 DIM/IDLE/SLEEP 降档）。
    // 典型使用者：导航、秒表、支付二维码展示。
    SCREEN = 1
};

// ========================================================
// 系统级 WakeLock 登记表
//
// 唯一的“谁在阻止系统睡眠”账本，供两侧共用：
//   1. 内核 PowerManager：降档与 tickless 深睡前查询，避免在 OTA、
//      BLE 同步、文件写入等事务中途断电或丢时间。
//   2. 用户态电源服务：capability 校验后代理 App 的申请/释放。
//
// 泄漏保护：每个锁可带超时（ms），由系统心跳 on_tick() 驱动过期回收。
// 一个忘记释放的 WakeLock 会让设备永久无法进入低功耗态、几小时耗尽电池，
// 因此超时是电源子系统的必需防线，而不是可选优化。
//
// 约束（遵循 AGENTS.md）：
//   - 全静态分配，无 new/malloc，容量编译期固定；
//   - O(MAX_HOLDERS) 定长扫描，无无界循环；
//   - acquire/release 不得在 ISR 中调用（会修改共享状态且无锁保护），
//     ISR 只可调用只读的 can_sleep() / holds_screen_lock()；
//   - 同一 holder 重复申请按引用计数叠加，达到 uint16 上限后拒绝而不是回绕。
// ========================================================
class WakeLockRegistry {
public:
    static constexpr int MAX_HOLDERS = 16;
    static constexpr uint32_t NO_TIMEOUT = 0xFFFFFFFFu;

    static WakeLockRegistry& instance() {
        static WakeLockRegistry registry;
        return registry;
    }

    bool acquire(uint32_t holder_id, WakeLockType type = WakeLockType::PARTIAL, uint32_t timeout_ms = NO_TIMEOUT) {
        Entry* free_slot = nullptr;
        for (int i = 0; i < MAX_HOLDERS; ++i) {
            Entry& e = entries_[i];
            if (e.active && e.holder_id == holder_id) {
                if (e.ref_count >= 0xFFFFu) {
                    return false; // 资源上限：拒绝而不是回绕
                }
                e.ref_count++;
                // 升级为更强的类型（SCREEN 覆盖 PARTIAL），并刷新超时窗口
                if (type == WakeLockType::SCREEN) {
                    e.type = WakeLockType::SCREEN;
                }
                e.remaining_ms = timeout_ms;
                total_locks_++;
                return true;
            }
            if (!e.active && free_slot == nullptr) {
                free_slot = &e;
            }
        }

        if (free_slot == nullptr) {
            return false; // 表满：新持有者无法登记
        }

        free_slot->holder_id = holder_id;
        free_slot->ref_count = 1;
        free_slot->type = type;
        free_slot->remaining_ms = timeout_ms;
        free_slot->active = true;
        total_locks_++;
        return true;
    }

    // 释放一次引用；引用归零后槽位回收。未登记的 holder 返回 false。
    bool release(uint32_t holder_id) {
        for (int i = 0; i < MAX_HOLDERS; ++i) {
            Entry& e = entries_[i];
            if (!e.active || e.holder_id != holder_id) {
                continue;
            }
            if (e.ref_count > 0) {
                e.ref_count--;
                if (total_locks_ > 0) {
                    total_locks_--;
                }
            }
            if (e.ref_count == 0) {
                clear_slot(e);
            }
            return true;
        }
        return false;
    }

    // 持有者退出/被杀时的一次性清理：释放其全部引用
    void release_all(uint32_t holder_id) {
        for (int i = 0; i < MAX_HOLDERS; ++i) {
            Entry& e = entries_[i];
            if (!e.active || e.holder_id != holder_id) {
                continue;
            }
            if (total_locks_ >= e.ref_count) {
                total_locks_ -= e.ref_count;
            } else {
                total_locks_ = 0;
            }
            clear_slot(e);
            return;
        }
    }

    // 系统心跳驱动：递减带超时的锁并回收过期项。
    // 返回本次因超时而释放的锁数量（可用于诊断泄漏）。
    int on_tick(uint32_t delta_ms) {
        int expired = 0;
        for (int i = 0; i < MAX_HOLDERS; ++i) {
            Entry& e = entries_[i];
            if (!e.active || e.remaining_ms == NO_TIMEOUT) {
                continue;
            }
            if (e.remaining_ms > delta_ms) {
                e.remaining_ms -= delta_ms;
                continue;
            }
            if (total_locks_ >= e.ref_count) {
                total_locks_ -= e.ref_count;
            } else {
                total_locks_ = 0;
            }
            clear_slot(e);
            expired++;
        }
        return expired;
    }

    bool is_held(uint32_t holder_id) const {
        for (int i = 0; i < MAX_HOLDERS; ++i) {
            if (entries_[i].active && entries_[i].holder_id == holder_id && entries_[i].ref_count > 0) {
                return true;
            }
        }
        return false;
    }

    // 是否存在任何阻止睡眠的锁（PARTIAL 与 SCREEN 都阻止）
    bool holds_any() const {
        return total_locks_ > 0;
    }

    // 是否存在要求屏幕保持点亮的锁
    bool holds_screen() const {
        for (int i = 0; i < MAX_HOLDERS; ++i) {
            if (entries_[i].active && entries_[i].type == WakeLockType::SCREEN && entries_[i].ref_count > 0) {
                return true;
            }
        }
        return false;
    }

    // 当前登记在案的引用总数（同一 holder 多次申请计多次）
    int lock_count() const {
        return total_locks_;
    }

    int holder_count() const {
        int count = 0;
        for (int i = 0; i < MAX_HOLDERS; ++i) {
            if (entries_[i].active) {
                count++;
            }
        }
        return count;
    }

    // 距离最近一个带超时的锁过期还剩多少 ms；无带超时的锁返回 NO_TIMEOUT
    uint32_t next_expiry_ms() const {
        uint32_t nearest = NO_TIMEOUT;
        for (int i = 0; i < MAX_HOLDERS; ++i) {
            const Entry& e = entries_[i];
            if (e.active && e.remaining_ms != NO_TIMEOUT && e.remaining_ms < nearest) {
                nearest = e.remaining_ms;
            }
        }
        return nearest;
    }

    void reset() {
        for (int i = 0; i < MAX_HOLDERS; ++i) {
            clear_slot(entries_[i]);
        }
        total_locks_ = 0;
    }

private:
    struct Entry {
        uint32_t holder_id;
        uint16_t ref_count;
        uint32_t remaining_ms;
        WakeLockType type;
        bool active;
    };

    WakeLockRegistry() : entries_{}, total_locks_(0) {
        reset();
    }

    WakeLockRegistry(const WakeLockRegistry&) = delete;
    WakeLockRegistry& operator=(const WakeLockRegistry&) = delete;

    static void clear_slot(Entry& e) {
        e.holder_id = 0;
        e.ref_count = 0;
        e.remaining_ms = NO_TIMEOUT;
        e.type = WakeLockType::PARTIAL;
        e.active = false;
    }

    Entry entries_[MAX_HOLDERS];
    int total_locks_;
};

#endif // AURORA_POWER_WAKE_LOCK_REGISTRY_HPP
