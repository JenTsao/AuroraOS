#ifndef AURORA_UI_OVERLAY_ROOT_VIEW_HPP
#define AURORA_UI_OVERLAY_ROOT_VIEW_HPP

// ============================================================
// ui/overlay_root_view.hpp — 浮层根容器
// ============================================================
//
// 解决的问题
// --------------------------------
// UiManager::render() 只认一个 root_view_，而当前 miband8 直接把
// ScreenNavigator 设为 root。想在页面栈之上再画一层系统级浮层（通知
// 横幅、来电全屏弹窗、系统 OSD…）时，两条看起来可行的路都走不通：
//
//   1. ScreenNavigator::add_child(overlay)
//      —— ScreenNavigator 覆写了 ViewGroup::draw()，只画 stack_ 里的
//         当前 Screen，**从不遍历 children_**。子节点既不会被绘制，
//         也不会进入 ViewGroup::handle_gesture 的分发链（它同样自己
//         处理手势后直接 return）。更糟的是 ~ViewGroup 会 delete
//         children_，而 ScreenNavigator 是函数内 static。
//   2. 在 WatchApp::on_frame_render() 里手工多画一次
//      —— 绕过了 UiManager 的 damage 累积与分带裁剪：浮层不在任何
//         damage 矩形内，band loop 会整帧跳过它；即便强画也必然
//         与 set_clip_rect 的裁剪区打架。
//
// 做法
// --------------------------------
// 包一层「薄根容器」：它自己就是普通 ViewGroup，children_ 按 add 顺序
// 绘制（后加者盖在先加者之上）。把 ScreenNavigator 作为 child 0、
// 通知浮层作为 child 1，再把这个容器交给 UiManager::set_root_view()。
// 父链随之变成 Screen -> ScreenNavigator -> OverlayRootView，
// OverlayRootView 的 parent_ 为 nullptr，damage 正好在它这里累积，
// 与 ViewGroup::invalidate_rect_world 的「仅根节点累积」约定一致。
//
// 所有权：本容器**不拥有**子节点。
//   子节点通常是静态对象（ScreenNavigator::instance()）或由应用侧
//   自行分配的对象。基类 ~ViewGroup 会 delete children_，所以析构时
//   先把所有子节点摘链（remove_child 只改 children_ 数组与 parent_，
//   不 delete），让基类析构时无事可做。这条约束必须在文档与调用方之间
//   保持一致：add 进来的东西必须比 OverlayRootView 活得久。
//
// 零成本：header-only、无动态分配、子节点上限沿用 ViewGroup 的 16。
// 仅 miband8 编译进固件（ui/ 是 INTERFACE 库，见 ui/CMakeLists.txt），
// 其余板卡固件体积零增量。
// ============================================================

#include "view_group.hpp"
#include "ui_config.hpp"

namespace UI {

class OverlayRootView : public ViewGroup {
public:
    OverlayRootView() : ViewGroup(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT) {}

    ~OverlayRootView() override {
        // 非拥有契约：摘链而非释放。索引 0 反复摘，避免 remove_child 造成的
        // 数组左移让循环漏掉元素。
        while (get_child_count() > 0) {
            remove_child(get_child(0));
        }
    }

    OverlayRootView(const OverlayRootView&) = delete;
    OverlayRootView& operator=(const OverlayRootView&) = delete;
};

} // namespace UI

#endif // AURORA_UI_OVERLAY_ROOT_VIEW_HPP
