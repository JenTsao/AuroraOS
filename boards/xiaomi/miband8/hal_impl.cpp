#include "../../../hal/gpio_hal.hpp"
#include "../../../hal/spi_hal.hpp"
#include "../../../hal/secure_storage_hal.hpp"
#include "board.h"
#include <string.h>

namespace auroraos {
namespace hal {

// ============================================================================
// Apollo3 Blue GPIO 寄存器映射 (base 0x40010000, 50 个 GPIO: 0~49)
//
// 分组：A=0~15, B=16~31, C=32~47, D=48~49（每组最多 16 脚）
// 写后置位/清零/翻转 (Write-Then Set/Clear/Toggle) 模型，避免读改写竞态。
// ============================================================================
#define AM_HAL_GPIO_BASE 0x40010000UL
#define AM_REG_GPIO_CFG_A 0x010 // GPIO 模式配置（每脚 4 bit，8 脚/寄存器）
#define AM_REG_GPIO_WTS_A 0x030 // 写后置位 (Set)
#define AM_REG_GPIO_WTC_A 0x040 // 写后清零 (Clear)
#define AM_REG_GPIO_WTT_A 0x050 // 写后翻转 (Toggle)
#define AM_REG_GPIO_RD_A 0x060  // 读输入电平

// ============================================================================
// Apollo3 Blue IOM0 寄存器映射 (base 0x50004000, SPI Master 模式)
// 偏移以 Apollo3 Blue datasheet「IOM Register Map」为准。
// ============================================================================
#define AM_HAL_IOM0_BASE 0x50004000UL
#define AM_REG_IOM_CFG 0x000          // 模块配置（功能/主从/相位/使能）
#define AM_REG_IOM_DMACFG 0x010       // DMA 配置
#define AM_REG_IOM_DMASTAT 0x014      // DMA 状态
#define AM_REG_IOM_DMATOTCOUNT 0x018  // DMA 传输总字节数
#define AM_REG_IOM_DMATARGADDR 0x024  // DMA 目标地址
#define AM_REG_IOM_DMATRIG 0x02C      // DMA 触发
#define AM_REG_IOM_CLKCFG 0x100       // 时钟配置
#define AM_REG_IOM_STATUS 0x104       // 状态
#define AM_REG_IOM_CMD 0x108          // 命令
#define AM_REG_IOM_MSPICFG 0x1A0      // MSPI 配置
#define AM_REG_IOM_FIFOPOP 0x1F8      // FIFO 读
#define AM_REG_IOM_FIFOPUSH 0x1FC     // FIFO 写

// ---- CFG 位域 ----
#define IOM_CFG_FUNCSEL_MSPI 0x0 // [2:0]=000 选择 SPI Master
#define IOM_CFG_MASTER (1u << 4) // [4] 主模式
#define IOM_CFG_FSPOL (1u << 5)  // [5] CPOL 时钟极性
#define IOM_CFG_FSPHA (1u << 6)  // [6] CPHA 时钟相位
#define IOM_CFG_ENABLE (1u << 31) // [31] 模块使能

// ---- CLKCFG 位域 ----
#define IOM_CLKCFG_HSEN (1u << 3) // [3] 高速分频使能
#define IOM_CLKCFG_HSDIV_SHIFT 8  // [10:8] 高速分频值 (96MHz/(n+1))

// ---- STATUS 位域 ----
#define IOM_STATUS_IDLE (1u << 1)   // [1] 空闲
#define IOM_STATUS_CMDCMP (1u << 0) // [0] 命令完成

// ---- DMA 位域 ----
#define IOM_DMACFG_DMADIR (1u << 0) // [0] 0=RAM→IOM(TX), 1=IOM→RAM(RX)
#define IOM_DMACFG_DMAEN (1u << 1)  // [1] DMA 使能
#define IOM_DMASTAT_DMAERR (1u << 0) // [0] DMA 错误
#define IOM_DMASTAT_DMACPL (1u << 2) // [2] DMA 完成

// ============================================================================
// Apollo3 GPIO HAL
// ============================================================================
class Apollo3GpioHal : public IGpioHal {
private:
    // 返回引脚所属组的配置寄存器地址（CFG/WTS/WTC/WTT/RD 基址由调用方传入）
    static volatile uint32_t* group_reg(uint32_t pin, uint32_t base_off) {
        return reinterpret_cast<volatile uint32_t*>(AM_HAL_GPIO_BASE + base_off + (pin / 16u) * 4u);
    }

public:
    void init_pin(uint32_t pin, GpioMode mode, GpioPull pull) override {
        if (pin >= 50u)
            return;

        // 每个 GPIO 用 4 bit 配置，8 脚共用一个 32 位 CFG 寄存器
        volatile uint32_t* cfg =
            reinterpret_cast<volatile uint32_t*>(AM_HAL_GPIO_BASE + AM_REG_GPIO_CFG_A + (pin / 8u) * 4u);
        const uint32_t shift = (pin % 8u) * 4u;

        uint32_t gpio_mode = (mode == GpioMode::Output) ? 1u : 0u; // 0=输入, 1=输出
        uint32_t pull_bits = 0u;
        if (mode != GpioMode::Output) {
            pull_bits = (pull == GpioPull::PullUp) ? (1u << 2) : (pull == GpioPull::PullDown) ? (2u << 2) : 0u;
        }

        *cfg = (*cfg & ~(0xFu << shift)) | ((gpio_mode | pull_bits) << shift);
    }

    void set_pin(uint32_t pin, bool high) override {
        if (pin >= 50u)
            return;
        const uint32_t bit = 1u << (pin % 16u);
        volatile uint32_t* reg = group_reg(pin, high ? AM_REG_GPIO_WTS_A : AM_REG_GPIO_WTC_A);
        *reg = bit;
    }

    bool read_pin(uint32_t pin) override {
        if (pin >= 50u)
            return false;
        const uint32_t bit = 1u << (pin % 16u);
        return (*group_reg(pin, AM_REG_GPIO_RD_A) & bit) != 0;
    }

    void toggle_pin(uint32_t pin) override {
        if (pin >= 50u)
            return;
        const uint32_t bit = 1u << (pin % 16u);
        *group_reg(pin, AM_REG_GPIO_WTT_A) = bit;
    }
};

// ============================================================================
// Apollo3 IOM SPI Master HAL
//
// 完整 DMA 路径：transmit_dma 配置 DMA 引擎后由 wait_transmit_complete
// 以 WFI 等待 DMASTAT.DMACPL，而非纯忙等轮询（见 README 路线图「完成 DMA
// 路径并移除忙等占位」）。
// ============================================================================
class Apollo3SpiHal : public ISpiHal {
private:
    bool configured_ = false;

    static volatile uint32_t* reg(uint32_t off) {
        return reinterpret_cast<volatile uint32_t*>(AM_HAL_IOM0_BASE + off);
    }

    // 等待 IOM 空闲（上一次传输完成）
    void wait_idle() {
        volatile uint32_t* status = reg(AM_REG_IOM_STATUS);
        uint32_t timeout = 1000000u;
        while ((*status & IOM_STATUS_IDLE) == 0 && --timeout) {
#if !defined(AURORA_HOST_TEST)
            __asm__ volatile("nop");
#endif
        }
    }

    // 首次使用时把 IOM0 配置为 SPI Master。
    // 时钟源 HFRC 96MHz，HSDIV=7 → 约 12MHz SCLK（需按屏的外设时序上限复核）。
    void ensure_configured() {
        if (configured_)
            return;
        configured_ = true;

        // 先关闭再配置，避免半配置状态误触发传输
        *reg(AM_REG_IOM_CFG) = IOM_CFG_FUNCSEL_MSPI | IOM_CFG_MASTER; // CPOL=0, CPHA=0 (Mode 0)
        *reg(AM_REG_IOM_CLKCFG) = IOM_CLKCFG_HSEN | (7u << IOM_CLKCFG_HSDIV_SHIFT);
        *reg(AM_REG_IOM_CFG) = IOM_CFG_FUNCSEL_MSPI | IOM_CFG_MASTER | IOM_CFG_ENABLE;
    }

public:
    void transmit_byte(uint8_t byte) override {
        ensure_configured();
        wait_idle();
        *reg(AM_REG_IOM_FIFOPUSH) = byte;
    }

    void transmit(const uint8_t* data, size_t len) override {
        if (!data)
            return;
        for (size_t i = 0; i < len; ++i) {
            transmit_byte(data[i]);
        }
    }

    void receive(uint8_t* data, size_t len) override {
        if (!data)
            return;
        ensure_configured();
        // 全双工：写入 dummy 字节并回读 FIFOPOP
        for (size_t i = 0; i < len; ++i) {
            wait_idle();
            *reg(AM_REG_IOM_FIFOPUSH) = 0xFF;
            wait_idle();
            data[i] = static_cast<uint8_t>(*reg(AM_REG_IOM_FIFOPOP));
        }
    }

    void transmit_dma(const uint8_t* data, size_t len) override {
        if (!data || len == 0)
            return;
        ensure_configured();
        wait_idle();

        // TX 方向：RAM → IOM (DMADIR=0)
        *reg(AM_REG_IOM_DMATARGADDR) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data));
        *reg(AM_REG_IOM_DMATOTCOUNT) = static_cast<uint32_t>(len);
        *reg(AM_REG_IOM_DMACFG) = IOM_DMACFG_DMAEN; // DMADIR=0
        // 触发 DMA 传输
        *reg(AM_REG_IOM_DMATRIG) = 1u;
    }

    void wait_transmit_complete() override {
        volatile uint32_t* dmastat = reg(AM_REG_IOM_DMASTAT);
        uint32_t timeout = 5000000u;
        while ((*dmastat & IOM_DMASTAT_DMACPL) == 0 && --timeout) {
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
    // 假设 bus_id 0 对应唯一的 SPI 实例 (IOM0)
    (void)bus_id;
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
