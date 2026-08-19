#ifndef AURORA_DRIVERS_OV2640_DRIVER_HPP
#define AURORA_DRIVERS_OV2640_DRIVER_HPP

#include "camera_driver.hpp"
#include "ov2640_regs.hpp"
#include "../../hal/i2c_hal.hpp"

namespace auroraos {
namespace drivers {
namespace camera {

class Ov2640Driver : public CameraDriverBase {
public:
    explicit Ov2640Driver(hal::II2cHal* i2c = nullptr, hal::ICameraHal* hal = nullptr, const char* name = "video0")
        : CameraDriverBase(name),
          i2c_(i2c),
          current_bank_(0xFF) {
        hal_ = hal;
    }

    void bind_i2c(hal::II2cHal* i2c) {
        i2c_ = i2c;
    }

    // 写入单个寄存器 (带 Bank 自动切换)
    bool write_reg(uint8_t reg, uint8_t val) {
        if (!i2c_) return false;
        return i2c_->write_reg(OV2640_I2C_ADDR, reg, &val, 1);
    }

    // 读取单个寄存器
    bool read_reg(uint8_t reg, uint8_t* out_val) {
        if (!i2c_ || !out_val) return false;
        return i2c_->read_reg(OV2640_I2C_ADDR, reg, out_val, 1);
    }

    // 切换寄存器 Bank
    bool set_bank(uint8_t bank) {
        if (current_bank_ == bank) return true;
        if (write_reg(BANK_SEL, bank)) {
            current_bank_ = bank;
            return true;
        }
        return false;
    }

    // 批量写入寄存器配置表
    bool write_reg_table(const RegVal* table) {
        if (!table) return false;
        while (table->reg != REG_TABLE_END || table->val != REG_TABLE_END) {
            if (table->reg == BANK_SEL) {
                set_bank(table->val);
            } else {
                if (!write_reg(table->reg, table->val)) {
                    return false;
                }
            }
            table++;
        }
        return true;
    }

protected:
    // =========================================================================
    // 传感器探测与硬件握手
    // =========================================================================
    bool probe_sensor() override {
        if (!i2c_) return false;

        // 1. 上电复位序列
        if (hal_) {
            hal_->set_power(true);
            hal_->hard_reset();
        }

        // 2. 切换到 Bank 1 探测芯片 ID
        if (!set_bank(BANK_SENSOR)) return false;

        uint8_t midh = 0, midl = 0, pidh = 0, pidl = 0;
        read_reg(SENSOR_MIDH, &midh);
        read_reg(SENSOR_MIDL, &midl);
        read_reg(SENSOR_PIDH, &pidh);
        read_reg(SENSOR_PIDL, &pidl);

        // 校验 OmniVision 厂商标识 (0x7FA2) 与 OV2640 产品 ID (0x2640 ~ 0x2642)
        if (midh != 0x7F || midl != 0xA2 || pidh != 0x26) {
            return false;
        }

        // 3. 执行默认寄存器初始化序列
        return write_reg_table(ov2640_init_regs);
    }

    // =========================================================================
    // 格式与分辨率配置
    // =========================================================================
    bool apply_format(uint16_t width, uint16_t height, PixelFormat fmt) override {
        if (!i2c_) return false;

        // 1. 切换输出模式
        switch (fmt) {
            case PixelFormat::RGB565:
                if (!write_reg_table(ov2640_rgb565_regs)) return false;
                break;
            case PixelFormat::YUV422:
                if (!write_reg_table(ov2640_yuv422_regs)) return false;
                break;
            case PixelFormat::JPEG:
                if (!write_reg_table(ov2640_jpeg_regs)) return false;
                break;
            default:
                return false;
        }

        // 2. 配置 DSP 缩放寄存器
        set_bank(BANK_DSP);
        write_reg(DSP_RESET, 0x04);
        write_reg(DSP_ZMOW, static_cast<uint8_t>((width >> 2) & 0xFF));
        write_reg(DSP_ZMOH, static_cast<uint8_t>((height >> 2) & 0xFF));
        write_reg(DSP_SIZEL, static_cast<uint8_t>(((width & 0x03) << 2) | (height & 0x03)));
        write_reg(DSP_RESET, 0x00);

        return true;
    }

    // =========================================================================
    // 图像调节与特效控制
    // =========================================================================
    bool apply_controls(const CameraControls& ctrl) override {
        if (!i2c_) return false;

        // 1. 镜像与翻转控制 (Bank 1)
        set_bank(BANK_SENSOR);
        uint8_t reg04 = 0x28; // 默认基准
        if (ctrl.hflip) reg04 |= 0x40; // bit6: HFLIP
        if (ctrl.vflip) reg04 |= 0x80; // bit7: VFLIP
        write_reg(SENSOR_REG04, reg04);

        // 2. 特效与色彩控制 (Bank 0)
        set_bank(BANK_DSP);

        // 特效滤镜
        uint8_t effect_val = 0x00;
        switch (ctrl.effect) {
            case SpecialEffect::Negative:  effect_val = 0x40; break;
            case SpecialEffect::Grayscale: effect_val = 0x18; break;
            case SpecialEffect::Reddish:   effect_val = 0x01; break;
            case SpecialEffect::Greenish:  effect_val = 0x02; break;
            case SpecialEffect::Bluish:    effect_val = 0x04; break;
            case SpecialEffect::Sepia:     effect_val = 0x10; break;
            default:                       effect_val = 0x00; break;
        }
        write_reg(DSP_BPADDR, 0x00);
        write_reg(DSP_BPDATA, effect_val);

        // 测试彩条图案 (DSP_TEST bit0)
        uint8_t test_val = ctrl.test_pattern ? 0x01 : 0x00;
        write_reg(DSP_TEST, test_val);

        return true;
    }

    // =========================================================================
    // 捕获流启动与停止
    // =========================================================================
    bool start_hardware_capture() override {
        if (!hal_) return true; // 若无硬件 HAL 则处于抽象就绪态

        size_t buf_sz = get_frame_byte_size(width_, height_, format_);
        return hal_->start_dma_capture(
            ping_buffer_, pong_buffer_, buf_sz,
            [](uint8_t* completed_buf, size_t len, void* ctx) {
                auto* self = static_cast<Ov2640Driver*>(ctx);
                self->on_dma_frame_completed(completed_buf, len);
            },
            this
        );
    }

    bool stop_hardware_capture() override {
        if (!hal_) return true;
        hal_->stop_dma_capture();
        return true;
    }

private:
    hal::II2cHal* i2c_;
    uint8_t current_bank_;
};

} // namespace camera
} // namespace drivers
} // namespace auroraos

#endif // AURORA_DRIVERS_OV2640_DRIVER_HPP
