#ifndef AURORA_UI_CONFIG_HPP
#define AURORA_UI_CONFIG_HPP

#ifdef AURORA_HOST_TEST
#define DISPLAY_WIDTH 192
#define DISPLAY_HEIGHT 490
#else
#include "board.h"
#endif

#include "../drivers/display/renderer2d.hpp"

namespace UI {
    // 统一 UI 引擎使用的 Renderer 类型，避免底层的模板参数污染上层业务逻辑
    // 在内存受限的板子上(如 miband8)，AURORA_FB_CHUNK_HEIGHT 会被 cmake 设置为 60
    // 以启用条带化渲染，节省约 165KB SRAM。
#ifndef AURORA_FB_CHUNK_HEIGHT
#define AURORA_FB_CHUNK_HEIGHT DISPLAY_HEIGHT
#endif
    using UIRenderer = Renderer2D<DISPLAY_WIDTH, AURORA_FB_CHUNK_HEIGHT>;
}

#endif // AURORA_UI_CONFIG_HPP
