#ifndef AURORA_CHARGING_MANAGER_HPP
#define AURORA_CHARGING_MANAGER_HPP

#include <stdint.h>

// 充电状态机
enum class ChargeState : uint8_t {
    DISCHARGING, // 电池供电
    PRE_CHARGE,  // 预充电 (低电压保护)
    FAST_CHARGE, // 恒流/恒压快充
    CHARGE_DONE, // 充满电
    FAULT        // 充电异常 (过温/过压)
};

enum class BatteryHealth : uint8_t {
    GOOD,
    OVERHEAT,
    COLD,
    OVER_VOLTAGE,
    DEAD,
    UNKNOWN
};

// ========================================================
// 抽象电池/充电IC驱动接口
// ========================================================
class BatteryDriver {
public:
    virtual ~BatteryDriver() = default;
    virtual bool init() = 0;
    virtual uint16_t read_voltage_mv() = 0;
    virtual bool is_vbus_plugged() = 0;
    virtual ChargeState get_charge_state() = 0;
    virtual int16_t read_temperature_c() { return 25; }
};

// ========================================================
// 模拟电池驱动 (供测试与 QEMU 使用)
// ========================================================
class MockBatteryDriver : public BatteryDriver {
private:
    uint16_t voltage_mv_;
    bool vbus_plugged_;
    ChargeState state_;
    int16_t temperature_c_;

public:
    MockBatteryDriver()
        : voltage_mv_(3800), vbus_plugged_(false), state_(ChargeState::DISCHARGING), temperature_c_(25) {}

    bool init() override {
        return true;
    }

    uint16_t read_voltage_mv() override {
        return voltage_mv_;
    }

    bool is_vbus_plugged() override {
        return vbus_plugged_;
    }

    ChargeState get_charge_state() override {
        return state_;
    }

    int16_t read_temperature_c() override {
        return temperature_c_;
    }

    // 测试专用辅助函数
    void set_voltage(uint16_t mv) {
        voltage_mv_ = mv;
    }

    void set_plugged(bool plugged) {
        vbus_plugged_ = plugged;
        if (!plugged) {
            state_ = ChargeState::DISCHARGING;
        } else if (state_ == ChargeState::DISCHARGING) {
            state_ = ChargeState::FAST_CHARGE;
        }
    }

    void set_state(ChargeState state) {
        state_ = state;
    }

    void set_temperature(int16_t temp_c) {
        temperature_c_ = temp_c;
    }
};

// ========================================================
// 充电与电量管理器
// ========================================================
class ChargingManager {
private:
    BatteryDriver* driver_;
    MockBatteryDriver default_mock_driver_;

    uint32_t poll_ticks_;
    static constexpr uint32_t POLL_INTERVAL_TICKS = 1000; // 每 1 秒轮询一次 (假设 1ms tick)

    uint16_t current_voltage_mv_;
    uint8_t current_soc_; // State of Charge (0-100%)
    int16_t current_temp_c_;
    bool is_plugged_;
    bool just_plugged_in_;     // VBUS 插入上升沿
    bool just_unplugged_;      // VBUS 拔出下降沿
    bool critical_low_active_; // 滞回状态机使用
    ChargeState charge_state_;
    BatteryHealth health_;

    ChargingManager()
        : driver_(&default_mock_driver_), poll_ticks_(0), current_voltage_mv_(0), current_soc_(0),
          current_temp_c_(25), is_plugged_(false), just_plugged_in_(false), just_unplugged_(false),
          critical_low_active_(false), charge_state_(ChargeState::DISCHARGING), health_(BatteryHealth::GOOD) {
        driver_->init();
        update_battery_status(); // 初始化时拉取一次
    }

    // 查表法计算电池电量百分比 (3.0V - 4.2V 锂电池放电曲线简化)
    uint8_t calculate_soc(uint16_t voltage_mv) {
        if (voltage_mv >= 4150)
            return 100;
        if (voltage_mv <= 3300)
            return 0; // 3.3V 视为空电强制关机阈值

        // 简单的分段线性插值 (实际中可根据电池具体放电曲线精调)
        if (voltage_mv > 3800) {
            // 3800mV ~ 4150mV -> 50% ~ 100%
            return 50 + ((voltage_mv - 3800) * 50) / 350;
        } else {
            // 3300mV ~ 3800mV -> 0% ~ 50%
            return ((voltage_mv - 3300) * 50) / 500;
        }
    }

    BatteryHealth evaluate_health(uint16_t voltage_mv, int16_t temp_c) {
        if (temp_c > 50) return BatteryHealth::OVERHEAT;
        if (temp_c < 0) return BatteryHealth::COLD;
        if (voltage_mv > 4350) return BatteryHealth::OVER_VOLTAGE;
        if (voltage_mv < 2800) return BatteryHealth::DEAD;
        return BatteryHealth::GOOD;
    }

    void update_battery_status() {
        if (!driver_)
            return;

        current_voltage_mv_ = driver_->read_voltage_mv();
        current_soc_ = calculate_soc(current_voltage_mv_);
        current_temp_c_ = driver_->read_temperature_c();
        health_ = evaluate_health(current_voltage_mv_, current_temp_c_);

        bool newly_plugged = driver_->is_vbus_plugged();

        // 边缘检测：累积边沿状态直到被上层 consume (读取并清除)
        if (!is_plugged_ && newly_plugged)
            just_plugged_in_ = true;
        if (is_plugged_ && !newly_plugged)
            just_unplugged_ = true;

        is_plugged_ = newly_plugged;
        charge_state_ = driver_->get_charge_state();

        // 温度保护：如果过温，切入故障保护
        if (health_ == BatteryHealth::OVERHEAT || health_ == BatteryHealth::OVER_VOLTAGE) {
            charge_state_ = ChargeState::FAULT;
        }

        // 滞回逻辑：低于 5% 触发，充到 8% 以上解除，防止状态抖动
        if (current_soc_ < 5) {
            critical_low_active_ = true;
        } else if (current_soc_ >= 8) {
            critical_low_active_ = false;
        }
    }

public:
    // Rule of Five (禁用拷贝)
    ChargingManager(const ChargingManager&) = delete;
    ChargingManager& operator=(const ChargingManager&) = delete;

    static ChargingManager& instance() {
        static ChargingManager manager;
        return manager;
    }

    static void early_init() {
        instance();
    }

    // 允许注入真实的硬件驱动
    void set_driver(BatteryDriver* driver) {
        if (driver) {
            driver_ = driver;
            driver_->init();
            update_battery_status();
        }
    }

    // 系统心跳级联调用
    void on_tick(uint32_t delta_ticks) {
        poll_ticks_ += delta_ticks;

        if (poll_ticks_ >= POLL_INTERVAL_TICKS) {
            poll_ticks_ = 0;
            update_battery_status();
        }
    }

    // ========================================================
    // 公开查询接口
    // ========================================================
    uint8_t get_soc() const {
        return current_soc_;
    }

    uint16_t get_voltage_mv() const {
        return current_voltage_mv_;
    }

    int16_t get_temperature_c() const {
        return current_temp_c_;
    }

    BatteryHealth get_battery_health() const {
        return health_;
    }

    bool is_plugged() const {
        return is_plugged_;
    }

    // 采用 Consume-on-read 语义，读取后自动清除标志位，防止事件丢失
    bool has_just_plugged() {
        if (just_plugged_in_) {
            just_plugged_in_ = false;
            return true;
        }
        return false;
    }

    bool has_just_unplugged() {
        if (just_unplugged_) {
            just_unplugged_ = false;
            return true;
        }
        return false;
    }

    ChargeState get_charge_state() const {
        return charge_state_;
    }

    // 用于提供给 PowerManager 的电量极度危险警告信号 (带有滞回区间)
    bool is_critical_low() const {
        return critical_low_active_ && !is_plugged_;
    }

    // 估算剩余续航时间（分钟），默认按 250mAh 电池与典型 15mA 放电电流计算
    uint32_t estimate_remaining_minutes(uint32_t battery_capacity_mah = 250, uint32_t current_draw_ma = 15) const {
        if (current_draw_ma == 0) return 0xFFFFFFFF;
        uint32_t remaining_mah = (current_soc_ * battery_capacity_mah) / 100;
        return (remaining_mah * 60) / current_draw_ma;
    }

    MockBatteryDriver* get_mock_driver() {
        return &default_mock_driver_;
    }
};

#endif // AURORA_CHARGING_MANAGER_HPP
