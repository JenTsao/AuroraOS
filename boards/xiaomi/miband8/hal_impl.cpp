#include "../../../hal/gpio_hal.hpp"
#include "../../../hal/spi_hal.hpp"

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

} // namespace hal
} // namespace auroraos
