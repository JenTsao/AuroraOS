#ifndef AURORA_UI_CONFIG_HPP
#define AURORA_UI_CONFIG_HPP

#ifdef AURORA_HOST_TEST
#define DISPLAY_WIDTH 192
#define DISPLAY_HEIGHT 490
#else
#include "board.h"
#endif

#include "../drivers/display/renderer2d.hpp"

// 统一 UI 引擎使用的 FrameBuffer 条带高度与 Renderer 类型
// 在内存受限的板子上(如 miband8)，AURORA_FB_CHUNK_HEIGHT 统一为 30
// 以启用条带化渲染，节省约 170KB SRAM。
#ifndef AURORA_FB_CHUNK_HEIGHT
#define AURORA_FB_CHUNK_HEIGHT 30
#endif

namespace UI {
using UIRenderer = Renderer2D<DISPLAY_WIDTH, AURORA_FB_CHUNK_HEIGHT>;
} // namespace UI

#endif // AURORA_UI_CONFIG_HPP
