#ifndef AURORA_NOTIFICATION_CENTER_HPP
#define AURORA_NOTIFICATION_CENTER_HPP

// ============================================================
// apps/notification_center.hpp — Aurora OS 通知中心
//
// 设计原则 (cpp-coding-standards):
//   - enum class: NotificationPriority/Category (Enum.3)
//   - constexpr: 所有魔数 (ES.45, Con.5)
//   - noexcept: 所有纯计算与数据操作 (F.6)
//   - [[nodiscard]]: 返回关键结果的函数
//   - Rule of Zero: 无自定义析构的数据类
//   - INotificationOverlay 抽象接口: DI 测试解耦 (cpp-testing)
//   - 零堆内存: Notification 全定长, Queue 静态数组, Overlay 静态内联
//   - 无 SF.7 违规: 头文件无 using namespace
// ============================================================

#include <stdint.h>
#include <stddef.h>
#include "../ui/view_group.hpp"
#include "watch/font_engine.hpp"

namespace aurora {

// ============================================================
// NotificationPriority (Enum.3: scoped, Con.5: compile-time ordering)
// ============================================================
enum class NotificationPriority : uint8_t {
    low      = 0,
    normal   = 1,
    high     = 2,
    critical = 3
};

// ============================================================
// NotificationCategory (Enum.3)
// ============================================================
enum class NotificationCategory : uint8_t {
    system  = 0,
    app     = 1,
    message = 2,
    call    = 3   // call + critical → 全屏弹窗
};

// ============================================================
// Notification — 定长通知数据包 (96 字节, 无动态内存)
// ============================================================
struct Notification {
    static constexpr uint8_t kTitleMaxLen = 16; // 含 null 终止符
    static constexpr uint8_t kBodyMaxLen  = 64; // 含 null 终止符

    uint32_t             id{0};
    NotificationPriority priority{NotificationPriority::normal};
    NotificationCategory category{NotificationCategory::app};
    char                 title[kTitleMaxLen]{};
    char                 body[kBodyMaxLen]{};
    uint32_t             timestamp{0};
    bool                 dismissed{false};
};

// ============================================================
// PriorityNotificationQueue — 静态最大堆 (Max-Heap)
// 比较规则: priority 高的优先; 相同 priority 时 timestamp 更大(更新)的优先
// ============================================================
class PriorityNotificationQueue {
public:
    static constexpr int kCapacity = 8;

    PriorityNotificationQueue() noexcept : size_{0} {}

    // C.20 Rule of Zero: 值类型成员，编译器自动生成拷贝/析构
    ~PriorityNotificationQueue()                                    = default;
    PriorityNotificationQueue(const PriorityNotificationQueue&)     = default;
    PriorityNotificationQueue& operator=(const PriorityNotificationQueue&) = default;

    // 入队。队列满时返回 false。(I.1: 显式接口)
    [[nodiscard]] bool push(const Notification& n) noexcept {
        if (size_ >= kCapacity) return false;
        heap_[size_] = n;
        sift_up(size_);
        ++size_;
        return true;
    }

    // 出队最高优先级通知。空队列时返回 false。
    [[nodiscard]] bool pop(Notification& out) noexcept {
        if (size_ == 0) return false;
        out = heap_[0];
        --size_;
        if (size_ > 0) {
            heap_[0] = heap_[size_];
            sift_down(0);
        }
        return true;
    }

    // 窥视堆顶 (非拥有原始指针, R.3)
    [[nodiscard]] const Notification* peek() const noexcept {
        return (size_ > 0) ? &heap_[0] : nullptr;
    }

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] int  size()  const noexcept { return size_; }

private:
    // ES.45: 具名比较函数，无魔数
    static bool has_higher_priority(const Notification& a,
                                    const Notification& b) noexcept
    {
        const uint8_t pa = static_cast<uint8_t>(a.priority);
        const uint8_t pb = static_cast<uint8_t>(b.priority);
        if (pa != pb) return pa > pb;
        return a.timestamp > b.timestamp; // 同优先级，更新的通知优先
    }

    void sift_up(int i) noexcept {
        while (i > 0) {
            const int parent = (i - 1) / 2;
            if (has_higher_priority(heap_[i], heap_[parent])) {
                swap_entries(heap_[i], heap_[parent]);
                i = parent;
            } else {
                break;
            }
        }
    }

    void sift_down(int i) noexcept {
        while (true) {
            int largest = i;
            const int left  = 2 * i + 1;
            const int right = 2 * i + 2;
            if (left  < size_ && has_higher_priority(heap_[left],  heap_[largest])) largest = left;
            if (right < size_ && has_higher_priority(heap_[right], heap_[largest])) largest = right;
            if (largest == i) break;
            swap_entries(heap_[i], heap_[largest]);
            i = largest;
        }
    }

    static void swap_entries(Notification& a, Notification& b) noexcept {
        Notification tmp = a;
        a = b;
        b = tmp;
    }

    Notification heap_[kCapacity];
    int          size_;
};

// ============================================================
// BleNotificationParser — 无状态 TLV 解包器 (纯函数, 可直接 GTest 注入)
//
// TLV 自定义报文格式 (ANCS/AMS 启发):
//   [Tag:1B][Len:1B][Value:Len B] ... (小端序)
//   0x01  ID (4 bytes LE)
//   0x02  Priority (1 byte, 0-3 → NotificationPriority)
//   0x03  Category (1 byte, 0-3 → NotificationCategory)
//   0x04  Title (variable, ≤ 15 chars + null)
//   0x05  Body  (variable, ≤ 63 chars + null)
// ============================================================
class BleNotificationParser {
public:
    static constexpr uint8_t kTagId       = 0x01;
    static constexpr uint8_t kTagPriority = 0x02;
    static constexpr uint8_t kTagCategory = 0x03;
    static constexpr uint8_t kTagTitle    = 0x04;
    static constexpr uint8_t kTagBody     = 0x05;
    static constexpr uint8_t kMaxPriorityVal  = 3;
    static constexpr uint8_t kMaxCategoryVal  = 3;

    // F.8: 纯函数 — 所有输入通过参数传递，无副作用，无全局状态访问
    [[nodiscard]] static Notification parse(
            const uint8_t* raw, uint8_t raw_len,
            uint32_t current_tick) noexcept
    {
        Notification n{};
        n.timestamp = current_tick;

        uint8_t i = 0;
        while (i + 2u <= raw_len) {            // 至少 Tag + Len
            const uint8_t tag     = raw[i];
            const uint8_t val_len = raw[i + 1];
            i += 2;

            if (static_cast<uint8_t>(i + val_len) > raw_len) break; // 截断保护

            switch (tag) {
                case kTagId:
                    if (val_len >= 4) {
                        n.id = decode_le32(raw + i);
                    }
                    break;

                case kTagPriority:
                    if (val_len >= 1 && raw[i] <= kMaxPriorityVal) {
                        n.priority = static_cast<NotificationPriority>(raw[i]);
                    }
                    break;

                case kTagCategory:
                    if (val_len >= 1 && raw[i] <= kMaxCategoryVal) {
                        n.category = static_cast<NotificationCategory>(raw[i]);
                    }
                    break;

                case kTagTitle:
                    safe_copy(n.title, Notification::kTitleMaxLen, raw + i, val_len);
                    break;

                case kTagBody:
                    safe_copy(n.body, Notification::kBodyMaxLen, raw + i, val_len);
                    break;

                default:
                    break; // 未知 TLV 标签：跳过，前向兼容
            }
            i += val_len;
        }
        return n;
    }

private:
    static uint32_t decode_le32(const uint8_t* p) noexcept {
        return static_cast<uint32_t>(p[0])
             | (static_cast<uint32_t>(p[1]) << 8)
             | (static_cast<uint32_t>(p[2]) << 16)
             | (static_cast<uint32_t>(p[3]) << 24);
    }

    static void safe_copy(char* dst, uint8_t dst_cap,
                          const uint8_t* src, uint8_t src_len) noexcept
    {
        const uint8_t max = (src_len < dst_cap - 1u) ? src_len
                                                      : static_cast<uint8_t>(dst_cap - 1u);
        for (uint8_t k = 0; k < max; ++k) dst[k] = static_cast<char>(src[k]);
        dst[max] = '\0';
    }
};

// ============================================================
// INotificationOverlay — 抽象接口 (DI 解耦, 供 GTest Mock 实现)
// C.35: 基类析构为 public virtual
// ============================================================
class INotificationOverlay {
public:
    virtual ~INotificationOverlay() = default;

    virtual void show(const Notification& n)    = 0;
    virtual void hide()                          = 0;
    [[nodiscard]] virtual bool is_visible() const noexcept = 0;
    virtual void tick(uint32_t delta_ms)         = 0;
};

// ============================================================
// NotificationOverlay — OLED 通知弹窗 (实现了 INotificationOverlay + UI::ViewGroup)
//
// 双模态:
//   Banner     (priority < critical): 顶部 80px, 3s 后自动 hide
//   Fullscreen (priority == critical || category == call): 覆盖全屏, 需手动 dismiss
//
// Z-Order 设计: 应作为 root_view 最后一个子节点添加，确保渲染在最上层
// ============================================================
class NotificationOverlay : public UI::ViewGroup, public INotificationOverlay {
public:
    static constexpr uint32_t   kBannerDurationMs = 3000;
    static constexpr uint16_t   kBannerHeight     = 80;
    static constexpr uint16_t   kBannerRadius     = 6;
    static constexpr ColorRGB565 kBgBanner        = 0x2965; // 深靛青
    static constexpr ColorRGB565 kBgCritical      = 0xC000; // 警告红
    static constexpr ColorRGB565 kColorPrimary    = 0xFFFF; // 白
    static constexpr ColorRGB565 kColorSecondary  = 0xC618; // 浅灰
    static constexpr ColorRGB565 kColorAccent     = 0x07E0; // 极光绿 (消息图标)

    enum class DisplayMode : uint8_t { hidden, banner, fullscreen };

    // C.46: explicit 单参数构造
    explicit NotificationOverlay(uint16_t screen_w, uint16_t screen_h) noexcept
        : UI::ViewGroup(0, 0, screen_w, kBannerHeight)
        , screen_w_{screen_w}, screen_h_{screen_h}
        , mode_{DisplayMode::hidden}
        , elapsed_ms_{0}
        , current_{}
    {}

    // C.67: 多态类禁止公开拷贝
    NotificationOverlay(const NotificationOverlay&)            = delete;
    NotificationOverlay& operator=(const NotificationOverlay&) = delete;

    // INotificationOverlay impl
    void show(const Notification& n) noexcept override {
        current_   = n;
        elapsed_ms_ = 0;

        const bool is_critical = (n.priority == NotificationPriority::critical)
                              || (n.category == NotificationCategory::call);

        if (is_critical) {
            mode_    = DisplayMode::fullscreen;
            height_  = screen_h_;
        } else {
            mode_    = DisplayMode::banner;
            height_  = kBannerHeight;
        }
        invalidate();
    }

    void hide() noexcept override {
        mode_ = DisplayMode::hidden;
        invalidate();
    }

    [[nodiscard]] bool is_visible() const noexcept override {
        return mode_ != DisplayMode::hidden;
    }

    void tick(uint32_t delta_ms) noexcept override {
        if (mode_ == DisplayMode::banner) {
            elapsed_ms_ += delta_ms;
            if (elapsed_ms_ >= kBannerDurationMs) {
                hide();
            }
        }
    }

    // 手势系统接口：滑走通知
    void dismiss() noexcept { hide(); }

    [[nodiscard]] DisplayMode get_mode() const noexcept { return mode_; }

    // UI::View impl
    void draw(UI::UIRenderer& renderer) override {
        if (mode_ == DisplayMode::hidden) return;

        const ColorRGB565 bg = (mode_ == DisplayMode::fullscreen)
                             ? kBgCritical : kBgBanner;

        // --- 背景: 圆角矩形 ---
        renderer.fill_round_rect(x_, y_, width_, height_, kBannerRadius, bg);

        // --- 左侧类型色块 (4px 宽) ---
        const ColorRGB565 tag_color = category_color(current_.category);
        renderer.fill_rect(x_, y_, 4, static_cast<uint16_t>(height_), tag_color);

        // --- Title (大字, scale=2) ---
        constexpr uint16_t kTitleX = 10;
        constexpr uint16_t kTitleY = 8;
        renderer.draw_string(
            static_cast<int16_t>(x_ + kTitleX),
            static_cast<int16_t>(y_ + kTitleY),
            current_.title, 2,
            kColorPrimary, bg,
            font5x7_data, 5, 7);

        // --- Body (小字, scale=1) ---
        constexpr uint16_t kBodyY = 32;
        renderer.draw_string(
            static_cast<int16_t>(x_ + kTitleX),
            static_cast<int16_t>(y_ + kBodyY),
            current_.body, 1,
            kColorSecondary, bg,
            font5x7_data, 5, 7);

        // 全屏模式额外显示操作提示
        if (mode_ == DisplayMode::fullscreen) {
            constexpr uint16_t kHintY = 400;
            renderer.draw_string(
                static_cast<int16_t>(x_ + 20),
                static_cast<int16_t>(y_ + kHintY),
                "SWIPE RIGHT TO DISMISS", 1,
                kColorSecondary, bg,
                font5x7_data, 5, 7);
        }
    }

private:
    static ColorRGB565 category_color(NotificationCategory cat) noexcept {
        switch (cat) {
            case NotificationCategory::call:    return 0xF81F; // 品红 = 来电
            case NotificationCategory::message: return 0x07E0; // 绿   = 消息
            case NotificationCategory::system:  return 0xFFE0; // 黄   = 系统
            default:                            return 0x001F; // 蓝   = 应用
        }
    }

    uint16_t    screen_w_;
    uint16_t    screen_h_;
    DisplayMode mode_;
    uint32_t    elapsed_ms_;
    Notification current_;
};

// ============================================================
// NotificationCenter — 全系统通知总线单例 (Facade)
//
// 数据流:
//   BleManager (0x04) → BleNotificationParser::parse()
//     → NotificationCenter::post()
//       → PriorityNotificationQueue::push()
//         → INotificationOverlay::show()
//
// 时钟驱动:
//   FrameScheduler / WatchApp::on_tick() → on_tick(delta_ms)
//     → INotificationOverlay::tick()
//       → [banner 自动消失] → dispatch_next()
// ============================================================
class NotificationCenter {
public:
    [[nodiscard]] static NotificationCenter& instance() noexcept {
        static NotificationCenter nc;
        return nc;
    }

    // 注册 UI 叠层 (WatchApp 初始化时调用, R.3: non-owning)
    void set_overlay(INotificationOverlay* overlay) noexcept {
        overlay_ = overlay;
    }

    // 接收新通知 (BLE/系统事件调用, 可在 ISR 安全的后台任务中调用)
    bool post(const Notification& n) noexcept {
        const bool queued = queue_.push(n);
        // 当前无通知显示时立即弹出最高优先级通知
        if (queued && overlay_ && !overlay_->is_visible()) {
            dispatch_next();
        }
        return queued;
    }

    // 手势/按键系统调用：消除当前通知，展示下一条
    void dismiss_current() noexcept {
        if (overlay_) overlay_->hide();
        dispatch_next();
    }

    // 时钟驱动 (由 WatchApp::on_tick 或 FrameScheduler 调用)
    void on_tick(uint32_t delta_ms) noexcept {
        if (!overlay_) return;
        const bool was_visible = overlay_->is_visible();
        overlay_->tick(delta_ms);
        // Banner 定时结束后弹出下一条
        if (was_visible && !overlay_->is_visible()) {
            dispatch_next();
        }
    }

    [[nodiscard]] int  pending_count() const noexcept { return queue_.size(); }
    [[nodiscard]] bool has_pending()   const noexcept { return !queue_.empty(); }

    void clear() noexcept {
        Notification dummy;
        while (queue_.pop(dummy)) {}
    }

private:
    NotificationCenter() noexcept : overlay_{nullptr} {}

    // C.67: 单例禁止拷贝
    NotificationCenter(const NotificationCenter&)            = delete;
    NotificationCenter& operator=(const NotificationCenter&) = delete;

    void dispatch_next() noexcept {
        if (!overlay_ || queue_.empty()) return;
        Notification n;
        if (queue_.pop(n)) {
            overlay_->show(n);
        }
    }

    PriorityNotificationQueue queue_;
    INotificationOverlay*     overlay_; // R.3: non-owning
};

} // namespace aurora

#endif // AURORA_NOTIFICATION_CENTER_HPP
