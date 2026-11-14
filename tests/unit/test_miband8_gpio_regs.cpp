// =============================================================================
// tests/unit/test_miband8_gpio_regs.cpp
//
// Apollo3 GPIO 寄存器映射的回归测试。覆盖 DOCS/KNOWN_ISSUES.md 的 F2/F3：
//   F2 上下拉位域错（旧实现把 pull 编码写进 CFG 的 OUTCFG/INTD）
//   F3 读分组错（旧实现按 pin/16 分四组，真实为 RDA/RDB 两组）
//
// 这些是纯计算测试：板级 hal_impl.cpp 只在交叉编译固件里编译，无法在 PC 上
// 实例化，因此用影子寄存器模型驱动同一套 constexpr 地址计算。
// =============================================================================
#include <gtest/gtest.h>
#include <array>
#include <cstdint>

#include "../../boards/xiaomi/miband8/apollo_gpio_regs.hpp"

namespace apollo = auroraos::board::miband8::apollo_gpio;

namespace {

struct ShadowMmio {
    std::array<uint32_t, 0x100u / 4u> words{};

    void write(uint32_t off, uint32_t value) {
        words[off / 4u] = value;
    }

    uint32_t read(uint32_t off) const {
        return words[off / 4u];
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 寄存器偏移：以 AmbiqSuite SDK 的 g_sRegdumpGPIO[] 为准
// ---------------------------------------------------------------------------
TEST(Miband8ApolloGpio, RegisterOffsetsMatchAmbiqSuite) {
    EXPECT_EQ(0x40010000u, apollo::kGpioBase);
    EXPECT_EQ(50u, apollo::kPinCount);
    EXPECT_EQ(0x000u, apollo::kPadregA);
    EXPECT_EQ(0x040u, apollo::kCfgA);
    EXPECT_EQ(0x060u, apollo::kPadkey);
    EXPECT_EQ(0x080u, apollo::kRda);
    EXPECT_EQ(0x084u, apollo::kRdb);
    EXPECT_EQ(0x088u, apollo::kWta);
    EXPECT_EQ(0x08Cu, apollo::kWtb);
    EXPECT_EQ(0x090u, apollo::kWtsa);
    EXPECT_EQ(0x094u, apollo::kWtsb);
    EXPECT_EQ(0x098u, apollo::kWtca);
    EXPECT_EQ(0x09Cu, apollo::kWtcb);
    EXPECT_EQ(0x73u, apollo::kPadkeyUnlock);
    EXPECT_EQ(0x00u, apollo::kPadkeyLock);
}

// 旧宏的五个偏移实际上分别落在 PADREGE / PADREGM / CFGA / CFGE / PADKEY，
// 其中「读输入」读的是 PADKEY 锁存值，而不是任何 GPIO 数据寄存器。
TEST(Miband8ApolloGpio, LegacyOffsetsWereNotGpioDataRegisters) {
    EXPECT_EQ(0x010u, apollo::padreg_offset(16u)); // 旧 CFG_A -> PADREGE
    EXPECT_EQ(0x030u, apollo::padreg_offset(48u)); // 旧 WTS_A -> PADREGM
    EXPECT_EQ(0x040u, apollo::cfg_offset(0u));     // 旧 WTC_A -> CFGA
    EXPECT_EQ(0x050u, apollo::cfg_offset(32u));    // 旧 WTT_A -> CFGE
    EXPECT_EQ(0x060u, apollo::kPadkey);            // 旧 RD_A  -> PADKEY
}

// ---------------------------------------------------------------------------
// PADREG / CFG 分组
// ---------------------------------------------------------------------------
TEST(Miband8ApolloGpio, PadregHoldsFourPadsPerWord) {
    EXPECT_EQ(0x000u, apollo::padreg_offset(0u));
    EXPECT_EQ(0x000u, apollo::padreg_offset(3u));
    EXPECT_EQ(0x004u, apollo::padreg_offset(4u));
    EXPECT_EQ(0x02Cu, apollo::padreg_offset(44u));
    EXPECT_EQ(0x030u, apollo::padreg_offset(48u));
    EXPECT_EQ(0x030u, apollo::padreg_offset(49u));

    EXPECT_EQ(0u, apollo::padreg_shift(0u));
    EXPECT_EQ(24u, apollo::padreg_shift(3u));
    EXPECT_EQ(0u, apollo::padreg_shift(4u));
    EXPECT_EQ(8u, apollo::padreg_shift(49u));
}

TEST(Miband8ApolloGpio, CfgHoldsEightPadsPerWord) {
    EXPECT_EQ(0x040u, apollo::cfg_offset(0u));
    EXPECT_EQ(0x040u, apollo::cfg_offset(7u));
    EXPECT_EQ(0x044u, apollo::cfg_offset(8u));
    EXPECT_EQ(0x058u, apollo::cfg_offset(48u));
    EXPECT_EQ(0x058u, apollo::cfg_offset(49u));

    EXPECT_EQ(0u, apollo::cfg_shift(0u));
    EXPECT_EQ(28u, apollo::cfg_shift(7u));
    EXPECT_EQ(0u, apollo::cfg_shift(8u));
    EXPECT_EQ(4u, apollo::cfg_shift(49u));
}

// ---------------------------------------------------------------------------
// F3：RDA 覆盖 0..31，RDB 覆盖 32..49；位号取 pin & 0x1F
// ---------------------------------------------------------------------------
TEST(Miband8ApolloGpio, ReadBankingUsesRdaFor0To31AndRdbFor32To49) {
    for (uint32_t pin = 0u; pin < 32u; ++pin) {
        EXPECT_EQ(apollo::kRda, apollo::ab_offset(pin, apollo::kRda)) << "pin " << pin;
        EXPECT_FALSE(apollo::is_high_bank(pin)) << "pin " << pin;
    }
    for (uint32_t pin = 32u; pin < apollo::kPinCount; ++pin) {
        EXPECT_EQ(apollo::kRdb, apollo::ab_offset(pin, apollo::kRda)) << "pin " << pin;
        EXPECT_TRUE(apollo::is_high_bank(pin)) << "pin " << pin;
    }
}

TEST(Miband8ApolloGpio, BankBitUsesLowerFiveBits) {
    EXPECT_EQ(1u << 0, apollo::ab_bit(0u));
    EXPECT_EQ(1u << 15, apollo::ab_bit(15u));
    EXPECT_EQ(1u << 16, apollo::ab_bit(16u));
    EXPECT_EQ(1u << 31, apollo::ab_bit(31u));
    EXPECT_EQ(1u << 0, apollo::ab_bit(32u));
    EXPECT_EQ(1u << 17, apollo::ab_bit(49u));
}

TEST(Miband8ApolloGpio, LegacySixteenPinGroupingWasWrongFromPin16) {
    const auto legacy_offset = [](uint32_t pin) {
        return 0x080u + (pin / 16u) * 4u;
    };
    const auto legacy_bit = [](uint32_t pin) {
        return 1u << (pin % 16u);
    };

    EXPECT_EQ(0x080u, legacy_offset(15u));
    EXPECT_EQ(0x084u, legacy_offset(16u)); // 旧实现把 pin16 放进 B 组
    EXPECT_EQ(0x088u, legacy_offset(32u)); // pin32 甚至落到 WTA(0x088)

    EXPECT_EQ(apollo::kRda, apollo::ab_offset(16u, apollo::kRda));
    EXPECT_EQ(1u << 16, apollo::ab_bit(16u));
    EXPECT_NE(legacy_bit(16u), apollo::ab_bit(16u));
    EXPECT_EQ(apollo::kRdb, apollo::ab_offset(32u, apollo::kRda));
}

TEST(Miband8ApolloGpio, WriteAndToggleRegistersUseTheSameBanking) {
    EXPECT_EQ(apollo::kWtsa, apollo::ab_offset(31u, apollo::kWtsa));
    EXPECT_EQ(apollo::kWtsb, apollo::ab_offset(32u, apollo::kWtsa));
    EXPECT_EQ(apollo::kWtca, apollo::ab_offset(31u, apollo::kWtca));
    EXPECT_EQ(apollo::kWtcb, apollo::ab_offset(32u, apollo::kWtca));
    EXPECT_EQ(apollo::kWta, apollo::ab_offset(31u, apollo::kWta));
    EXPECT_EQ(apollo::kWtb, apollo::ab_offset(32u, apollo::kWta));
}

// ---------------------------------------------------------------------------
// F2：PADREG 位域与 CFG 位域
// ---------------------------------------------------------------------------
TEST(Miband8ApolloGpio, PadregFieldEncodesGpioInputAndPullup) {
    // FNCSEL=3(GPIO) << 3 | INPEN | PULL = 0x18 | 0x02 | 0x01
    EXPECT_EQ(0x1Bu, apollo::padreg_field(true, true));
    EXPECT_EQ(0x1Au, apollo::padreg_field(true, false));
    EXPECT_EQ(0x18u, apollo::padreg_field(false, false));
    EXPECT_EQ(0x19u, apollo::padreg_field(false, true));
    EXPECT_EQ(0x18u | (2u << 6), apollo::padreg_field(false, false, 3u, 2u));
}

TEST(Miband8ApolloGpio, LegacyPullEncodingHitCfgOutcfgAndIntd) {
    const uint32_t legacy_pullup = 1u << 2;   // 旧 PullUp
    const uint32_t legacy_pulldown = 2u << 2; // 旧 PullDown == 1u << 3

    EXPECT_EQ(0x2u, (legacy_pullup >> apollo::kCfgOutcfgShift) & 0x3u); // OUTCFG=开漏
    EXPECT_EQ(1u, (legacy_pulldown >> apollo::kCfgIntdShift) & 1u);     // INTD=1

    // 新实现：CFG 里不含任何上下拉位，上下拉只出现在 PADREG 的 bit0
    EXPECT_EQ(0u, apollo::cfg_field(true) & legacy_pullup);
    EXPECT_EQ(0u, apollo::cfg_field(false) & legacy_pulldown);
    EXPECT_EQ(1u, apollo::padreg_field(true, true) & (1u << apollo::kPadPullShift));
}

TEST(Miband8ApolloGpio, CfgFieldEncodesPushpullInBits2And1) {
    EXPECT_EQ(0x0u, apollo::cfg_field(false));
    EXPECT_EQ(0x2u, apollo::cfg_field(true)); // OUTCFG=1(推挽) << 1
}

// ---------------------------------------------------------------------------
// 影子寄存器：用同一套地址计算走一遍 init / read，确认不会串脚、串寄存器
// ---------------------------------------------------------------------------
TEST(Miband8ApolloGpio, ShadowMmioPin16AndPin32DoNotAlias) {
    ShadowMmio mmio;
    const uint32_t off16 = apollo::ab_offset(16u, apollo::kRda);
    const uint32_t off32 = apollo::ab_offset(32u, apollo::kRda);
    ASSERT_NE(off16, off32);

    mmio.write(off16, apollo::ab_bit(16u));
    mmio.write(off32, apollo::ab_bit(32u));

    EXPECT_EQ(1u << 16, mmio.read(apollo::kRda));
    EXPECT_EQ(1u << 0, mmio.read(apollo::kRdb));
    EXPECT_EQ(0u, mmio.read(apollo::kRda) & 1u); // pin32 不会误置 pin0
}

TEST(Miband8ApolloGpio, ShadowMmioInitPinWritesPadregCfgAndRelocksPadkey) {
    ShadowMmio mmio;
    const uint32_t pin = 6u; // board.h 的 PIN_BUTTON_SIDE

    mmio.write(apollo::kPadkey, apollo::kPadkeyUnlock);
    const uint32_t poff = apollo::padreg_offset(pin);
    const uint32_t pshift = apollo::padreg_shift(pin);
    mmio.write(poff, (mmio.read(poff) & ~(apollo::kPadFieldMask << pshift)) |
                         ((apollo::padreg_field(true, true) & apollo::kPadFieldMask) << pshift));
    const uint32_t coff = apollo::cfg_offset(pin);
    const uint32_t cshift = apollo::cfg_shift(pin);
    mmio.write(coff, (mmio.read(coff) & ~(apollo::kCfgFieldMask << cshift)) |
                         ((apollo::cfg_field(false) & apollo::kCfgFieldMask) << cshift));
    mmio.write(apollo::kPadkey, apollo::kPadkeyLock);

    EXPECT_EQ(0x004u, poff); // PADREGB (pads 4-7)
    EXPECT_EQ(16u, pshift);
    EXPECT_EQ(0x1Bu, (mmio.read(poff) >> pshift) & apollo::kPadFieldMask);
    EXPECT_EQ(0u, (mmio.read(coff) >> cshift) & apollo::kCfgFieldMask);
    EXPECT_EQ(apollo::kPadkeyLock, mmio.read(apollo::kPadkey));
    EXPECT_EQ(0u, mmio.read(apollo::kRda)); // 配置写入未污染输入数据寄存器
}

TEST(Miband8ApolloGpio, UpdatePadregFncselTargetsBits5To3AndPreservesOtherFields) {
    // 预设同一 PADREGB 中 pad4/pad5/pad6 的 PULL/INPEN/RSEL 位
    const uint32_t initial = 0xC7u << 8 | 0x45u; // pad5=0xC7 (RSEL=3, STRNG=1, INPEN=1, PULL=1)
    const uint32_t updated = apollo::update_padreg_fncsel(initial, 5u, 1u);

    // pad5 bit[5:3] 改为 1 (0x08)，其余 bit[7:6]=0xC0 与 bit[2:0]=0x07 保持不变 -> 0xCF
    EXPECT_EQ(0xCFu, (updated >> apollo::padreg_shift(5u)) & 0xFFu);
    // 同字的 pad4 保持原值 0x45
    EXPECT_EQ(0x45u, (updated >> apollo::padreg_shift(4u)) & 0xFFu);
    // 旧 board.cpp 误写 (1u << 8) 实际命中 bit[0] (PULL) 而非 FNCSEL(bit[5:3])
    EXPECT_NE(0x1u << 8, (1u << 3) << apollo::padreg_shift(5u));
}
