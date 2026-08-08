#ifndef AURORA_NOTIFICATION_CENTER_HPP
#define AURORA_NOTIFICATION_CENTER_HPP

// ============================================================
// apps/notification_center.hpp — Aurora OS 通知中心
// ============================================================

#include <stdint.h>
#include <stddef.h>
#include "../ui/view_group.hpp"
#include "watch/font_engine.hpp"

namespace aurora {

enum class NotificationPriority : uint8_t {
    low      = 0,
    normal   = 1,
    high     = 2,
    critical = 3
};

enum class NotificationCategory : uint8_t {
    system  = 0,
    app     = 1,
    message = 2,
    call    = 3   // call + critical → 全屏弹窗
};

struct Notification {
    static constexpr uint8_t kTitleMaxLen = 16;
    static constexpr uint8_t kBodyMaxLen  = 64;

    uint32_t             id{0};
    NotificationPriority priority{NotificationPriority::normal};
    NotificationCategory category{NotificationCategory::app};
    char                 title[kTitleMaxLen]{};
    char                 body[kBodyMaxLen]{};
    uint32_t             timestamp{0};
    bool                 dismissed{false};
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
    [[nodiscard]] int  size()  const noexcept;

private:
    static bool has_higher_priority(const Notification& a, const Notification& b) noexcept;
    void sift_up(int i) noexcept;
    void sift_down(int i) noexcept;
    static void swap_entries(Notification& a, Notification& b) noexcept;

    Notification heap_[kCapacity];
    int          size_;
};

class BleNotificationParser {
public:
    static constexpr uint8_t kTagId       = 0x01;
    static constexpr uint8_t kTagPriority = 0x02;
    static constexpr uint8_t kTagCategory = 0x03;
    static constexpr uint8_t kTagTitle    = 0x04;
    static constexpr uint8_t kTagBody     = 0x05;
    static constexpr uint8_t kMaxPriorityVal  = 3;
    static constexpr uint8_t kMaxCategoryVal  = 3;

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

class NotificationOverlay : public UI::ViewGroup, public INotificationOverlay {
public:
    static constexpr uint32_t   kBannerDurationMs = 3000;
    static constexpr uint16_t   kBannerHeight     = 80;
    static constexpr uint16_t   kBannerRadius     = 6;
    static constexpr ColorRGB565 kBgBanner        = 0x2965;
    static constexpr ColorRGB565 kBgCritical      = 0xC000;
    static constexpr ColorRGB565 kColorPrimary    = 0xFFFF;
    static constexpr ColorRGB565 kColorSecondary  = 0xC618;
    static constexpr ColorRGB565 kColorAccent     = 0x07E0;

    enum class DisplayMode : uint8_t { hidden, banner, fullscreen };

    explicit NotificationOverlay(uint16_t screen_w, uint16_t screen_h) noexcept;
    NotificationOverlay(const NotificationOverlay&) = delete;
    NotificationOverlay& operator=(const NotificationOverlay&) = delete;

    void show(const Notification& n) noexcept override;
    void hide() noexcept override;
    [[nodiscard]] bool is_visible() const noexcept override;
    void tick(uint32_t delta_ms) noexcept override;
    void dismiss() noexcept;
    [[nodiscard]] DisplayMode get_mode() const noexcept;
    void draw(UI::UIRenderer& renderer) override;

private:
    static ColorRGB565 category_color(NotificationCategory cat) noexcept;

    uint16_t    screen_w_;
    uint16_t    screen_h_;
    DisplayMode mode_;
    uint32_t    elapsed_ms_;
    Notification current_;
};

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
    INotificationOverlay*     overlay_;
};

} // namespace aurora

#endif // AURORA_NOTIFICATION_CENTER_HPP
