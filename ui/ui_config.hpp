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
    using UIRenderer = Renderer2D<DISPLAY_WIDTH, DISPLAY_HEIGHT>;
}

#endif // AURORA_UI_CONFIG_HPP
