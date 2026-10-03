#ifndef AURORA_UI_CONFIG_HPP
#define AURORA_UI_CONFIG_HPP

#ifdef AURORA_HOST_TEST
#define DISPLAY_WIDTH 192
#define DISPLAY_HEIGHT 490
#else
#include "board.h"
#endif

#include "../drivers/display/renderer2d.hpp"

// =============================================================================
// 条带（band）渲染的条带高度
//
// 内存受限的板子（如 miband8，384KB SRAM）无法承受整屏 192x490x2B = 184KB 的
// 帧缓冲，改用「一次只渲染若干行、逐条带推屏」的条带化策略，可节省约 170KB SRAM。
//
// 默认取 35：miband8 的 490 = 14 x 35 精确整除 → 14 条带、无残带。
// 板级若通过 -D 覆盖（CONFIG 化），以板级值为准。
// =============================================================================
#ifndef AURORA_FB_CHUNK_HEIGHT
#define AURORA_FB_CHUNK_HEIGHT 35
#endif

// 面板比「请求条带高」还小时，裁到面板高度。
// 目前唯一命中的板卡是 lm3s6965-qb（板载 SSD1306 只有 96x16）：此时退化为
// 「单带 = 整屏」。UiManager 分带循环里的 rows = min(band_h, DISPLAY_HEIGHT - top)
// 守卫是二层保险。
//
// ⚠️ 这里刻意**不再** `#undef`/重定义 AURORA_FB_CHUNK_HEIGHT：合规项 B-04 要求
// 该宏在 ui_config.hpp 内**有且只有一处 `#define`**。因此把它拆成两个名字：
//   AURORA_FB_CHUNK_HEIGHT —— 板级可覆盖的「请求值」（唯一 #define）
//   AURORA_UI_BAND_H        —— 实际生效值（请求值与面板高的较小者）
// 所有 FrameBuffer<W, H> 与 UIRenderer 必须统一用 AURORA_UI_BAND_H，
// 否则渲染器与帧缓冲高度不一致会直接编译失败。
#if AURORA_FB_CHUNK_HEIGHT > DISPLAY_HEIGHT
#define AURORA_UI_BAND_H DISPLAY_HEIGHT
#else
#define AURORA_UI_BAND_H AURORA_FB_CHUNK_HEIGHT
#endif

// 全局安全断言（对所有板卡成立）：
//   - 生效条带高必须为正，否则 FrameBuffer<W, 0> 是零长数组；
//   - 生效条带高不得超过面板高度，否则单条带就装不下整屏 → 越界写显存。
static_assert(AURORA_UI_BAND_H > 0, "AURORA_UI_BAND_H must be positive");
static_assert(AURORA_UI_BAND_H <= DISPLAY_HEIGHT, "AURORA_UI_BAND_H must not exceed the panel height");

// 注意：整除性断言刻意不放在这里 —— 其它板卡的面板高（如 nucleo 的 128）未必被
// 35 整除，那种板卡靠分带循环的残带守卫正确处理即可。整除性是 miband8 的板级
// 约束，断言放在只被 miband8 编译的 apps/watch/watch_app.cpp 里。

namespace UI {
using UIRenderer = Renderer2D<DISPLAY_WIDTH, AURORA_UI_BAND_H>;
} // namespace UI

#endif // AURORA_UI_CONFIG_HPP
