#include "../../../hal/gpio_hal.hpp"
#include "../../../hal/spi_hal.hpp"
#include "../../../hal/secure_storage_hal.hpp"
#include "board.h"
#include <string.h>

namespace auroraos {
namespace hal {

// Apollo3 GPIO / SPI 寄存器地址
#define AM_HAL_GPIO_BASE 0x40010000
#define AM_HAL_GPIO_WT_EN (AM_HAL_GPIO_BASE + 0x04)
#define AM_HAL_GPIO_WT_DIS (AM_HAL_GPIO_BASE + 0x08)

#define AM_HAL_IOM_BASE 0x50004000
#define AM_HAL_IOM_FIFO (AM_HAL_IOM_BASE + 0x200)
#define AM_HAL_IOM_CMD (AM_HAL_IOM_BASE + 0x108)
#define AM_HAL_IOM_STATUS (AM_HAL_IOM_BASE + 0x104)

#define AM_HAL_IOM_DMA_CFG (AM_HAL_IOM_BASE + 0x2A0)
#define AM_HAL_IOM_DMA_TARG (AM_HAL_IOM_BASE + 0x2A4)
#define AM_HAL_IOM_DMA_TOTLEN (AM_HAL_IOM_BASE + 0x2A8)

class Apollo3GpioHal : public IGpioHal {
public:
    void init_pin(uint32_t pin, GpioMode mode, GpioPull pull) override {
        // 配置 Apollo3 GPIO 引脚
    }

    void set_pin(uint32_t pin, bool high) override {
        volatile uint32_t* gpio_wt_en = reinterpret_cast<uint32_t*>(AM_HAL_GPIO_WT_EN);
        volatile uint32_t* gpio_wt_dis = reinterpret_cast<uint32_t*>(AM_HAL_GPIO_WT_DIS);
        if (high) {
            *gpio_wt_en = (1 << pin);
        } else {
            *gpio_wt_dis = (1 << pin);
        }
    }

    bool read_pin(uint32_t pin) override {
        return false; // Not implemented yet
    }

    void toggle_pin(uint32_t pin) override {
        // Not implemented yet
    }
};

class Apollo3SpiHal : public ISpiHal {
public:
    void transmit_byte(uint8_t byte) override {
        volatile uint32_t* iom_fifo = reinterpret_cast<uint32_t*>(AM_HAL_IOM_FIFO);
        volatile uint32_t* iom_cmd = reinterpret_cast<uint32_t*>(AM_HAL_IOM_CMD);
        volatile uint32_t* iom_status = reinterpret_cast<uint32_t*>(AM_HAL_IOM_STATUS);

        *iom_fifo = byte;
        *iom_cmd = 0x1;
        uint32_t timeout = 1000000;
        while ((*iom_status & 0x1) != 0 && --timeout)
            ;
    }

    void transmit(const uint8_t* data, size_t len) override {
        for (size_t i = 0; i < len; ++i) {
            transmit_byte(data[i]);
        }
    }

    void receive(uint8_t* data, size_t len) override {
        // Not implemented yet
    }

    void transmit_dma(const uint8_t* data, size_t len) override {
        volatile uint32_t* dma_cfg = reinterpret_cast<uint32_t*>(AM_HAL_IOM_DMA_CFG);
        volatile uint32_t* dma_targ = reinterpret_cast<uint32_t*>(AM_HAL_IOM_DMA_TARG);
        volatile uint32_t* dma_totlen = reinterpret_cast<uint32_t*>(AM_HAL_IOM_DMA_TOTLEN);

        *dma_targ = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data));
        *dma_totlen = len;
        *dma_cfg = 0x1; // Enable DMA
    }

    void wait_transmit_complete() override {
        volatile uint32_t* iom_status = reinterpret_cast<uint32_t*>(AM_HAL_IOM_STATUS);
        uint32_t timeout = 5000000;
        while ((*iom_status & 0x2) == 0 && --timeout) {
#if !defined(AURORA_HOST_TEST)
            __asm__ volatile("wfi" : : : "memory");
#endif
        }
    }
};

IGpioHal* get_gpio_hal() {
    static Apollo3GpioHal gpio_hal;
    return &gpio_hal;
}

ISpiHal* get_spi_hal(int bus_id) {
    // 假设 bus_id 0 对应唯一的 SPI 实例
    static Apollo3SpiHal spi_hal;
    return &spi_hal;
}

// ========================================================
// Apollo3 Secure Storage — customer OTP 密钥读取
//
// 从 customer OTP 区域读取每设备唯一的 SoftBus 预共享密钥。
// 未烧录 (全 0xFF) 或魔术字不匹配时 fail-closed 返回 false，
// 绝不回退到默认/共享密钥。
// ========================================================
class Apollo3SecureStorageHal : public ISecureStorageHal {
public:
    bool is_provisioned() override {
        return read_record(/*out_key=*/nullptr, /*out_version=*/nullptr);
    }

    bool read_softbus_key(uint8_t key[32], uint32_t* version) override {
        return read_record(key, version);
    }

private:
    // 从 OTP 读取密钥记录并校验魔术字与烧录状态。
    // out_key / out_version 可为 nullptr（仅探测 provision 状态）。
    bool read_record(uint8_t* out_key, uint32_t* out_version) {
        const volatile uint8_t* otp =
            reinterpret_cast<const volatile uint8_t*>(OTP_CUSTOMER_BASE + OTP_SOFTBUS_KEY_OFFSET);

        // 读魔术字 (小端)
        uint32_t magic = 0;
        for (int i = 0; i < 4; ++i)
            magic |= static_cast<uint32_t>(otp[i]) << (8 * i);

        // 未烧录时 OTP 为全 0xFF；魔术字不匹配即视为未供应。
        if (magic != OTP_SOFTBUS_KEY_MAGIC)
            return false;

        // 读版本 (小端)
        uint32_t ver = 0;
        for (int i = 0; i < 4; ++i)
            ver |= static_cast<uint32_t>(otp[4 + i]) << (8 * i);

        // 读 32 字节密钥
        if (out_key) {
            for (int i = 0; i < 32; ++i)
                out_key[i] = otp[8 + i];
        }
        if (out_version)
            *out_version = ver;
        return true;
    }
};

ISecureStorageHal* get_secure_storage_hal() {
    static Apollo3SecureStorageHal secure;
    return &secure;
}

} // namespace hal
} // namespace auroraos
