#include "notification_center.hpp"

namespace aurora {

// ============================================================
// PriorityNotificationQueue Implementation
// ============================================================

PriorityNotificationQueue::PriorityNotificationQueue() noexcept : size_{0} {}

bool PriorityNotificationQueue::push(const Notification& n) noexcept {
    if (size_ >= kCapacity)
        return false;
    heap_[size_] = n;
    sift_up(size_);
    ++size_;
    return true;
}

bool PriorityNotificationQueue::pop(Notification& out) noexcept {
    if (size_ == 0)
        return false;
    out = heap_[0];
    --size_;
    if (size_ > 0) {
        heap_[0] = heap_[size_];
        sift_down(0);
    }
    return true;
}

const Notification* PriorityNotificationQueue::peek() const noexcept {
    return (size_ > 0) ? &heap_[0] : nullptr;
}

bool PriorityNotificationQueue::empty() const noexcept {
    return size_ == 0;
}

int PriorityNotificationQueue::size() const noexcept {
    return size_;
}

bool PriorityNotificationQueue::has_higher_priority(const Notification& a, const Notification& b) noexcept {
    const uint8_t pa = static_cast<uint8_t>(a.priority);
    const uint8_t pb = static_cast<uint8_t>(b.priority);
    if (pa != pb)
        return pa > pb;
    return a.timestamp > b.timestamp;
}

void PriorityNotificationQueue::sift_up(int i) noexcept {
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

void PriorityNotificationQueue::sift_down(int i) noexcept {
    while (true) {
        int largest = i;
        const int left = 2 * i + 1;
        const int right = 2 * i + 2;
        if (left < size_ && has_higher_priority(heap_[left], heap_[largest]))
            largest = left;
        if (right < size_ && has_higher_priority(heap_[right], heap_[largest]))
            largest = right;
        if (largest == i)
            break;
        swap_entries(heap_[i], heap_[largest]);
        i = largest;
    }
}

void PriorityNotificationQueue::swap_entries(Notification& a, Notification& b) noexcept {
    Notification tmp = a;
    a = b;
    b = tmp;
}

// ============================================================
// BleNotificationParser Implementation
// ============================================================

Notification BleNotificationParser::parse(const uint8_t* raw, uint8_t raw_len, uint32_t current_tick) noexcept {
    Notification n{};
    n.timestamp = current_tick;

    uint8_t i = 0;
    while (i + 2u <= raw_len) {
        const uint8_t tag = raw[i];
        const uint8_t val_len = raw[i + 1];
        i += 2;

        if (static_cast<uint8_t>(i + val_len) > raw_len)
            break;

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
            break;
        }
        i += val_len;
    }
    return n;
}

uint32_t BleNotificationParser::decode_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void BleNotificationParser::safe_copy(char* dst, uint8_t dst_cap, const uint8_t* src, uint8_t src_len) noexcept {
    const uint8_t max = (src_len < dst_cap - 1u) ? src_len : static_cast<uint8_t>(dst_cap - 1u);
    for (uint8_t k = 0; k < max; ++k)
        dst[k] = static_cast<char>(src[k]);
    dst[max] = '\0';
}

// ============================================================
// NotificationCenter Implementation
// ============================================================

NotificationCenter& NotificationCenter::instance() noexcept {
    static NotificationCenter nc;
    return nc;
}

NotificationCenter::NotificationCenter() noexcept : overlay_{nullptr} {}

void NotificationCenter::set_overlay(INotificationOverlay* overlay) noexcept {
    overlay_ = overlay;
}

bool NotificationCenter::post(const Notification& n) noexcept {
    const bool queued = queue_.push(n);
    if (queued && overlay_ && !overlay_->is_visible()) {
        dispatch_next();
    }
    return queued;
}

void NotificationCenter::dismiss_current() noexcept {
    if (overlay_)
        overlay_->hide();
    dispatch_next();
}

void NotificationCenter::on_tick(uint32_t delta_ms) noexcept {
    if (!overlay_)
        return;
    const bool was_visible = overlay_->is_visible();
    overlay_->tick(delta_ms);
    // cppcheck-suppress [incorrectLogicOperator, oppositeExpression]
    // was_visible snapshots visibility BEFORE tick(); tick() may hide a banner on
    // timeout (see apps/watch/notification_overlay_view.hpp ::tick), so this
    // conjunction is NOT always false.
    if (was_visible && !overlay_->is_visible()) {
        dispatch_next();
    }
}

int NotificationCenter::pending_count() const noexcept {
    return queue_.size();
}

bool NotificationCenter::has_pending() const noexcept {
    return !queue_.empty();
}

void NotificationCenter::clear() noexcept {
    Notification dummy;
    while (queue_.pop(dummy)) {}
}

void NotificationCenter::dispatch_next() noexcept {
    if (!overlay_ || queue_.empty())
        return;
    Notification n;
    if (queue_.pop(n)) {
        overlay_->show(n);
    }
}

} // namespace aurora
