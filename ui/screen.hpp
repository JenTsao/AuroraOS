#ifndef AURORA_UI_SCREEN_HPP
#define AURORA_UI_SCREEN_HPP

#include "view_group.hpp"
#include "ui_config.hpp"

namespace UI {

// ========================================================
// Screen: 页面基类
//
// 继承自 ViewGroup，代表一块占据整个屏幕的显示区域。
// 提供生命周期钩子，供 ScreenNavigator 在路由与过渡动画期间调用。
// ========================================================
class Screen : public ViewGroup {
public:
    // 默认全屏大小
    Screen() : ViewGroup(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT) {}
    
    virtual ~Screen() = default;

    // ========================================================
    // 生命周期钩子 (Lifecycle Hooks)
    // ========================================================
    
    // 当页面被 push 到栈顶，但在动画开始前触发（用于初始化数据）
    virtual void on_create() {}

    // 当页面动画结束，完全展示给用户时触发（用于开启传感器、定时器等）
    virtual void on_show() {}

    // 当页面准备被 pop 出栈，或者被新页面覆盖时触发（用于停止高频刷新、释放资源）
    virtual void on_hide() {}

    // 当页面被完全移出栈且即将被销毁时触发
    virtual void on_destroy() {}

    // ========================================================
    // 默认背景绘制
    // ========================================================
    // 默认情况下，Screen 应该填充背景，防止透明导致的残影。
    // 子类可以通过覆盖此方法，或者直接在 draw() 开头调用 UIRenderer 清屏。
    void draw(UIRenderer& renderer) override {
        // 默认用纯黑背景填充整个屏幕
        renderer.fill_rect(x_, y_, width_, height_, 0x0000);
        
        // 绘制子节点
        ViewGroup::draw(renderer);
    }
};

} // namespace UI

#endif // AURORA_UI_SCREEN_HPP
