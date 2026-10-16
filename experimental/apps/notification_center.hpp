#ifndef AURORA_NOTIFICATION_CENTER_HPP
#define AURORA_NOTIFICATION_CENTER_HPP

// ============================================================
// apps/notification_center.hpp — Aurora OS 通知中心
// ============================================================
//
// 分层约定（本次改动的核心）
// --------------------------------
// 本文件**刻意保持 UI 无关**：只包含 <stdint.h>/<stddef.h>，不引入任何
// UI 头文件。历史上这里 include 的是 experimental/ui/view_group.hpp ——
// 那是**主机单测专用 stub**（其 UIRenderer/ViewGroup 都是空实现），而固件
// 侧真实控件树是 ui/view.hpp + ui/view_group.hpp。两个头文件在同一个
// namespace UI 下各自声明 UIRenderer / ViewGroup，固件与主机两侧同时可见
// 时直接构成 ODR 冲突（一个是 class，一个是 ui_config.hpp 里的
// `using UIRenderer = Renderer2D<...>` 别名）。
//
// 于是 NotificationOverlay 只能继承 stub：它没有真实控件树的 add_child /
// invalidate / damage 冒泡 / 手势命中测试，产出的通知在真机上既画不出来
// 也收不掉。
//
// 现在的切分：
//   本文件      —— 纯逻辑：优先级队列 + BLE TLV 解析 + 通知中心调度，
//                  零 UI 依赖，主机/固件共用同一份实现。
//   INotificationOverlay —— 表现层端口（依赖倒置），由固件侧
//                  apps/watch/notification_overlay_view.hpp 的真实
//                  UI::View 子类实现。
//
// 参见 apps/watch/notification_overlay_view.hpp 与 ui/overlay_root_view.hpp。

#include <stdint.h>
#include <stddef.h>

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
    call = 3 // call + critical → 全屏弹窗
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

    [[nodiscard]] bool push(const Notification& n) noexcept;
    [[nodiscard]] bool pop(Notification& out) noexcept;
    [[nodiscard]] const Notification* peek() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] int size() const noexcept;

private:
    static bool has_higher_priority(const Notification& a, const Notification& b) noexcept;
    void sift_up(int i) noexcept;
    void sift_down(int i) noexcept;
    static void swap_entries(Notification& a, Notification& b) noexcept;

    Notification heap_[kCapacity];
    int size_;
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
// 调度语义（已被单测固定，见 experimental/tests/unit/test_notification_center.cpp）：
//   post()            入队；若当前无展示则立刻弹出队首
//   dismiss_current() 收起当前，并弹出下一条
//   on_tick()         驱动 banner 超时；超时收起后自动弹出下一条
//
// 本类不持有 overlay 的所有权，只持有一个非拥有裸指针（对齐仓库「非拥有
// 引用」惯例）。固件侧的生命周期由 WatchApp 负责。
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
