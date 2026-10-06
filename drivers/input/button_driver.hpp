// =============================================================================
// drivers/input/button_driver.hpp
//
// 物理按键（miband8 侧键）轮询驱动
//
//   - 轮询模型：HAL 无 GPIO 中断能力（hal/gpio_hal.hpp 仅 init/set/read/
//     toggle），与触摸 poll_input 同节奏，由 UI 线程独占调用
//     （单线程约定见 apps/watch/watch_app.cpp / miband_kernel.hpp）。
//   - 事件模型：Linux 输入风格，InputEvent.type=EV_KEY，
//     value: 1=down, 0=up, 2=repeat（第一个 repeat 即"长按达成"信号）。
//     短按与长按共用 KeyCode::KEY_POWER，语义区分靠 value + 时序：
//     code 标识"哪个物理键"，value 标识"怎么按"。
//   - 事件产出：内部环形缓冲（FIFO）。poll() 只推进状态机并入队，
//     pop_event() 出队；UI 消费与 VFS read() 共享同一队列。
//     这与 Gt316Driver 的"返回值带出"是刻意分歧：按键生命周期跨息屏
//     后台 tick，一次 poll 可能产出多个事件（长按 repeat、快速连按的
//     down+up 对），单事件返回值会丢；且 poll/取数解耦后 VFS read()
//     无需伪造时间戳（对比 gt316_driver.hpp read() 的 last+33 hack）。
//   - 策略与事实分离：本驱动只产出"电平事实"（down/up/repeat），
//     短按/长按 → 动作的映射在消费层（WatchApp::handle_key_event）。
//
// 设计原则（遵循 AGENTS.md 内核/驱动规范）：
//   - 零动态内存分配：对象内定长队列（8 x 12B = 96B）
//   - 通过 auroraos::hal::IGpioHal 抽象接口解耦硬件
//   - 时间戳一律由调用方传入 poll(now_ms)（可测试性关键，对齐 poll_touch）
//   - 所有时间比较基于无符号减法，tick 回绕（2^32 ms ≈ 49.7 天）安全
// =============================================================================
#ifndef AURORA_BUTTON_DRIVER_HPP
#define AURORA_BUTTON_DRIVER_HPP

#include <stdint.h>
#include <stddef.h>
#include "../../kernel/core/device.hpp"
#include "../../hal/gpio_hal.hpp"
#include "input_event.hpp"

class ButtonDriver : public CharDevice {
public:
    // 可调参数（模式对齐 GestureRecognizer 的可调阈值风格）
    static constexpr uint32_t kDefaultDebounceMs = 30;
    // 与 GestureRecognizer::THRESHOLD_LONG_PRESS_MS（gesture_recognizer.hpp:77）
    // 对齐为 800ms，保持全系统"长按"体感一致。刻意【不】include 手势层头：
    // 驱动层依赖手势层是反向依赖，仅注释对齐 + 同值。手势层阈值运行时可调，
    // 两处独立配置是有意为之（触摸长按与按键长按允许不同标定）。
    static constexpr uint32_t kDefaultLongPressMs = 800;
    static constexpr uint32_t kRepeatDisabled = 0;   // 0 = 禁用长按重复（v1 默认）
    static constexpr uint8_t  kQueueCapacity = 8;

    explicit ButtonDriver(const char* name = "button0")
        : CharDevice(name),
          gpio_(nullptr),
          pin_(0),
          active_high_(false),
          debounce_ms_(kDefaultDebounceMs),
          long_press_ms_(kDefaultLongPressMs),
          repeat_interval_ms_(kRepeatDisabled),
          state_(BtnState::kStable),
          stable_pressed_(false),
          candidate_pressed_(false),
          candidate_start_ms_(0),
          press_start_ms_(0),
          long_press_fired_(false),
          next_repeat_ms_(0),
          head_(0),
          count_(0),
          dropped_events_(0),
          has_injected_level_(false),
          injected_level_(false) {}

    // 单例访问（模式对齐 Gt316Driver::instance()）。
    // -fno-threadsafe-statics 下无守卫锁，安全性依赖"仅 UI 线程访问"的
    // 既有单线程独占约定，与 Gt316Driver 同一约束。
    static ButtonDriver& instance() {
        static ButtonDriver s_instance;
        return s_instance;
    }

    // 配置。与 Gt316Driver::configure 一致：只存指针，引脚初始化在 open()。
    // active_high：按键按下时引脚电平。默认 false = 低电平有效，
    // 此时 open() 配内部上拉（空闲高、按下拉低）；高有效则配下拉。
    void configure(auroraos::hal::IGpioHal* gpio,
                   uint32_t pin,
                   bool active_high = false,
                   uint32_t debounce_ms = kDefaultDebounceMs,
                   uint32_t long_press_ms = kDefaultLongPressMs) {
        gpio_ = gpio;
        pin_ = pin;
        active_high_ = active_high;
        debounce_ms_ = debounce_ms;
        long_press_ms_ = long_press_ms;
    }

    // 长按重复间隔；0=禁用（默认）。v1 不启用，接口预留。
    void set_repeat_interval_ms(uint32_t ms) { repeat_interval_ms_ = ms; }

    // ========================================================
    // 核心状态机：采样 → 消抖 → down/up/repeat 事件入队
    // 一次 poll 至多提交一次稳态转移；repeat 事件按时间阈值逐个补发。
    // ========================================================
    void poll(uint32_t now_ms) {
        const bool raw = sample_level();

        if (state_ == BtnState::kStable && raw != stable_pressed_) {
            state_ = BtnState::kDebouncing;
            candidate_pressed_ = raw;
            candidate_start_ms_ = now_ms;
        } else if (state_ == BtnState::kDebouncing) {
            if (raw != candidate_pressed_) {
                // 电平回弹：换候选电平、重新计时（连续抖动永不提交）
                candidate_pressed_ = raw;
                candidate_start_ms_ = now_ms;
            } else if (now_ms - candidate_start_ms_ >= debounce_ms_) {
                state_ = BtnState::kStable;
                if (raw != stable_pressed_) {
                    // 仅在稳态真正翻转时产事件；回弹回原稳态不产事件，
                    // 避免"抖回来后在 debounce 到期时误发无效 up/down"。
                    stable_pressed_ = raw;
                    if (raw) {
                        commit_press(now_ms);
                    } else {
                        commit_release(now_ms);
                    }
                }
            }
        }

        if (stable_pressed_) {
            if (!long_press_fired_ && now_ms - press_start_ms_ >= long_press_ms_) {
                push_event(now_ms, 2);  // 首个 repeat = "长按达成"
                long_press_fired_ = true;
                next_repeat_ms_ = now_ms + repeat_interval_ms_;
            } else if (long_press_fired_ && repeat_interval_ms_ != 0 &&
                       static_cast<int32_t>(now_ms - next_repeat_ms_) >= 0) {
                // 回绕安全的符号比较（无符号差转 int32 判序）
                push_event(now_ms, 2);
                next_repeat_ms_ += repeat_interval_ms_;
            }
        }
    }

    // 取出一个事件（FIFO）。无事件返回 false。UI 与 VFS read 共用。
    bool pop_event(InputEvent* out) {
        if (!out || count_ == 0)
            return false;
        *out = queue_[head_];
        head_ = static_cast<uint8_t>((head_ + 1) % kQueueCapacity);
        count_--;
        return true;
    }

    // ========================================================
    // 测试注入：直接驱动"原始电平"输入，绕过 HAL（对齐 inject_touch 思路，
    // 但注入的是持续电平而非一次性事件——可模拟按住不放）
    // ========================================================
    void inject_press() {
        has_injected_level_ = true;
        injected_level_ = true;
    }

    void inject_release() {
        has_injected_level_ = true;
        injected_level_ = false;
    }

    void clear_injection() { has_injected_level_ = false; }

    // 可观测性：队列满时丢弃的最旧事件计数
    uint32_t dropped_events() const { return dropped_events_; }

    // ========================================================
    // VFS 接口 (CharDevice)
    // ========================================================
    int open() override {
        if (gpio_) {
            // 极性-上拉对应：低有效按键（按下拉低）配内部上拉，空闲态读高；
            // 高有效按键配内部下拉，空闲态读低。
            gpio_->init_pin(pin_, auroraos::hal::GpioMode::Input,
                            active_high_ ? auroraos::hal::GpioPull::PullDown
                                         : auroraos::hal::GpioPull::PullUp);
        }
        reset_state();
        return 0;
    }

    int close() override {
        reset_state();
        return 0;
    }

    int read(char* buf, int len, int offset, void* priv) override {
        (void)offset;
        (void)priv;
        if (!buf || len < static_cast<int>(sizeof(InputEvent)))
            return 0;
        InputEvent ev;
        if (!pop_event(&ev))
            return 0;
        // InputEvent trivially copyable（touch_abi.hpp 守卫已断言）
        *reinterpret_cast<InputEvent*>(buf) = ev;
        return sizeof(InputEvent);
    }

private:
    enum class BtnState : uint8_t { kStable, kDebouncing };

    bool sample_level() const {
        if (has_injected_level_)
            return injected_level_;
        if (!gpio_)
            return stable_pressed_;  // 未 configure：维持稳态，永不产事件
        return gpio_->read_pin(pin_) == active_high_;
    }

    void commit_press(uint32_t now_ms) {
        push_event(now_ms, 1);  // down
        press_start_ms_ = now_ms;
        long_press_fired_ = false;
        next_repeat_ms_ = 0;
    }

    void commit_release(uint32_t now_ms) {
        // 长按后抬起仍发 up，保持 down, repeat..., up 的完整序列：
        // 消费方不会出现"按键悬空按下"状态。
        push_event(now_ms, 0);
    }

    void push_event(uint32_t now_ms, int32_t value) {
        const uint8_t tail = static_cast<uint8_t>((head_ + count_) % kQueueCapacity);
        queue_[tail].timestamp = now_ms;
        queue_[tail].type = InputEventType::EV_KEY;
        queue_[tail].code = static_cast<uint16_t>(KeyCode::KEY_POWER);
        queue_[tail].value = value;
        if (count_ == kQueueCapacity) {
            // 队列满：丢最旧，计数可观测（dropped_events()）
            head_ = static_cast<uint8_t>((head_ + 1) % kQueueCapacity);
            dropped_events_++;
        } else {
            count_++;
        }
    }

    void reset_state() {
        state_ = BtnState::kStable;
        stable_pressed_ = false;
        long_press_fired_ = false;
        head_ = 0;
        count_ = 0;
    }

    auroraos::hal::IGpioHal* gpio_;
    uint32_t pin_;
    bool active_high_;
    uint32_t debounce_ms_;
    uint32_t long_press_ms_;
    uint32_t repeat_interval_ms_;

    BtnState state_;
    bool stable_pressed_;
    bool candidate_pressed_;
    uint32_t candidate_start_ms_;
    uint32_t press_start_ms_;
    bool long_press_fired_;
    uint32_t next_repeat_ms_;

    InputEvent queue_[kQueueCapacity];
    uint8_t head_;
    uint8_t count_;
    uint32_t dropped_events_;

    bool has_injected_level_;
    bool injected_level_;
};

#endif // AURORA_BUTTON_DRIVER_HPP
