#ifndef AURORA_NOTIFICATION_CENTER_HPP
#define AURORA_NOTIFICATION_CENTER_HPP

// ============================================================
// services/notification/notification_center.hpp — AuroraOS 通知中心服务
// ============================================================
//
// 分层约定
// --------------------------------
// 本文件保持 UI 无关：只包含 <stdint.h>/<stddef.h>，不引入任何 UI 头文件。
//   - 本文件 (services/notification/) —— 纯逻辑：优先级堆队列、满队列关键告警抢占、
//     BLE TLV 解析与通知中心调度，零 UI 依赖，主机/固件共用同一份实现。
//   - INotificationOverlay —— 表现层端口（依赖倒置），由固件侧
//     apps/watch/notification_overlay_view.hpp 的真实 UI::View 子类实现，
//     并经 ui/overlay_root_view.hpp 挂载到控件树根部。

#include <stddef.h>
#include <stdint.h>

namespace aurora {

enum class NotificationPriority : uint8_t {
    low = 0,
    normal = 1,
    high = 2,
    critical = 3
};

enum class NotificationCategory : uint8_t {
    system = 0,
    app = 1,
    message = 2,
    call = 3 // call + critical → 全屏模态弹窗并享受满队列抢占保底
};

struct Notification {
    static constexpr uint8_t kTitleMaxLen = 16;
    static constexpr uint8_t kBodyMaxLen = 64;

    uint32_t id{0};
    NotificationPriority priority{NotificationPriority::normal};
    NotificationCategory category{NotificationCategory::app};
    char title[kTitleMaxLen]{};
    char body[kBodyMaxLen]{};
    uint32_t timestamp{0};
    bool dismissed{false};
};

class PriorityNotificationQueue {
public:
    static constexpr int kCapacity = 8;

    PriorityNotificationQueue() noexcept;
    ~PriorityNotificationQueue() = default;
    PriorityNotificationQueue(const PriorityNotificationQueue&) = default;
    PriorityNotificationQueue& operator=(const PriorityNotificationQueue&) = default;

    [[nodiscard]] static constexpr bool is_critical_alert(const Notification& n) noexcept {
        return (n.priority == NotificationPriority::critical) ||
               (n.category == NotificationCategory::call);
    }

    [[nodiscard]] static constexpr uint8_t effective_priority(const Notification& n) noexcept {
        return is_critical_alert(n)
                   ? static_cast<uint8_t>(NotificationPriority::critical)
                   : static_cast<uint8_t>(n.priority);
    }

    /**
     * 入队通知。
     * - 未满（size < kCapacity）：直接入堆并返回 true。
     * - 已满（size == kCapacity）：
     *   - 若新通知为关键告警（priority == critical 或 category == call），
     *     扫描堆中优先级最低（同优先级取最旧 timestamp）的非关键通知并淘汰替换，
     *     保底关键告警不丢失，递增 evicted_count() 并返回 true；
     *   - 若堆内全部为关键告警，或新通知本身为非关键通知，拒绝入队，
     *     递增 dropped_count() 并返回 false。
     */
    [[nodiscard]] bool push(const Notification& n) noexcept;
    [[nodiscard]] bool pop(Notification& out) noexcept;
    [[nodiscard]] const Notification* peek() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] int size() const noexcept;
    void clear() noexcept;

    [[nodiscard]] uint32_t evicted_count() const noexcept { return evicted_count_; }
    [[nodiscard]] uint32_t dropped_count() const noexcept { return dropped_count_; }

private:
    static bool has_higher_priority(const Notification& a, const Notification& b) noexcept;
    void sift_up(int i) noexcept;
    void sift_down(int i) noexcept;
    static void swap_entries(Notification& a, Notification& b) noexcept;

    Notification heap_[kCapacity]{};
    int size_{0};
    uint32_t evicted_count_{0};
    uint32_t dropped_count_{0};
};

class BleNotificationParser {
public:
    static constexpr uint8_t kTagId = 0x01;
    static constexpr uint8_t kTagPriority = 0x02;
    static constexpr uint8_t kTagCategory = 0x03;
    static constexpr uint8_t kTagTitle = 0x04;
    static constexpr uint8_t kTagBody = 0x05;
    static constexpr uint8_t kMaxPriorityVal = 3;
    static constexpr uint8_t kMaxCategoryVal = 3;

    [[nodiscard]] static Notification parse(const uint8_t* raw, uint8_t raw_len, uint32_t current_tick) noexcept;

private:
    static uint32_t decode_le32(const uint8_t* p) noexcept;
    static void safe_copy(char* dst, uint8_t dst_cap, const uint8_t* src, uint8_t src_len) noexcept;
};

class INotificationOverlay {
public:
    virtual ~INotificationOverlay() = default;

    virtual void show(const Notification& n) = 0;
    virtual void hide() = 0;
    [[nodiscard]] virtual bool is_visible() const noexcept = 0;
    virtual void tick(uint32_t delta_ms) = 0;
};

// ============================================================
// NotificationCenter —— 唯一对外入口（单例）
//
// 调度语义（见 tests/unit/test_notification_center.cpp）：
//   post()            入队（满队列时关键告警自动淘汰最低优先级非关键通知）；
//                     若当前无展示则立刻弹出队首
//   dismiss_current() 收起当前，并弹出下一条
//   on_tick()         驱动 banner 超时；超时收起后自动弹出下一条
//
// 本类不持有 overlay 的所有权，只持有一个非拥有裸指针。
// ============================================================

class NotificationCenter {
public:
    [[nodiscard]] static NotificationCenter& instance() noexcept;

    void set_overlay(INotificationOverlay* overlay) noexcept;
    bool post(const Notification& n) noexcept;
    void dismiss_current() noexcept;
    void on_tick(uint32_t delta_ms) noexcept;
    [[nodiscard]] int pending_count() const noexcept;
    [[nodiscard]] bool has_pending() const noexcept;
    [[nodiscard]] uint32_t evicted_count() const noexcept;
    [[nodiscard]] uint32_t dropped_count() const noexcept;
    void clear() noexcept;

private:
    NotificationCenter() noexcept;
    NotificationCenter(const NotificationCenter&) = delete;
    NotificationCenter& operator=(const NotificationCenter&) = delete;

    void dispatch_next() noexcept;

    PriorityNotificationQueue queue_;
    INotificationOverlay* overlay_;
};

} // namespace aurora

#endif // AURORA_NOTIFICATION_CENTER_HPP
