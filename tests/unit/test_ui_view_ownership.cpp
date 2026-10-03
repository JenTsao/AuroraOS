// =============================================================================
// test_ui_view_ownership.cpp — View 回调槽的所有权契约（缺陷 A 回归）
//
// 缺陷 A：旧版 ~View() 无条件 ::operator delete(on_click_ctx_)，把
//   「ctx == 正在析构的 Screen 自身」这种借用语义变成对活对象的二次释放：
//   pop → delete old_top → ~ViewGroup → delete children_[i] → ~View
//   → ::operator delete(正在析构的 Screen)。实测为「同一指针被释放多次」的组合 UB。
//
// 修复后契约（见 ui/view.hpp ListenerSlot）：
//   deleter == nullptr ⇒ 借用：ctx 归调用方，View 析构不得释放；
//   deleter != nullptr ⇒ 拥有：View 析构、或槽被覆盖时调用 deleter(ctx)。
// 同时旧版只释放 click 槽，long/double/touch 的 ctx 从未释放（既有泄漏），
// 本文件对其一并做回归。
// =============================================================================

#include <gtest/gtest.h>

#include "../../ui/ui_config.hpp"
#include "../../ui/view.hpp"
#include "../../ui/view_group.hpp"
#include "../../ui/widgets/scroll_view.hpp"

namespace {

// 文件内静态帧缓冲：test_lua_vm.cpp 已占用全局符号 g_fb（全部测试链接成
// 单一可执行文件），此处必须置于匿名命名空间以避免重名链接错误。
FrameBuffer<DISPLAY_WIDTH, AURORA_FB_CHUNK_HEIGHT> s_fb;

// 记录释放次数的「拥有型」ctx：通过自定义 deleter 释放，便于精确计数。
struct OwnedCtx {
    int id = 0;

    static int freed;
    static void destroy(void* p) {
        ++freed;
        delete static_cast<OwnedCtx*>(p);
    }
};
int OwnedCtx::freed = 0;

// 析构计数器：用于证明「借用自己的对象」只被析构一次。
struct CountingView : public UI::ViewGroup {
    using UI::ViewGroup::ViewGroup;

    static int destroyed;
    ~CountingView() override {
        ++destroyed;
    }
};
int CountingView::destroyed = 0;

void noop_click(UI::View*, void*) {}
void noop_long(UI::View*, void*) {}
void noop_double(UI::View*, void*) {}
bool noop_touch(UI::View*, const GestureEvent&, void*) {
    return false;
}

// 验证 ClipScope 在提前 return 路径上也能还原裁剪区（RAII）。
void draw_and_early_return(UI::UIRenderer& r) {
    UI::UIRenderer::ClipScope scope(r);
    r.set_clip_rect(1, 1, 3, 3);
    return; // 提前返回：scope 析构仍应把裁剪区还原到入栈值
}

} // namespace

// 借用语义负向断言：ctx == 自身（无 deleter）时析构不得释放，且只析构一次。
// 旧代码在此对活对象执行 ::operator delete(this) + delete-expression 的二次
// 释放（进程异常终止 / 旧代码必 FAIL）。
TEST(ViewCallbackOwnership, BorrowedSelfCtxIsNotFreed) {
    CountingView::destroyed = 0;
    CountingView* v = new CountingView(0, 0, 10, 10);
    v->set_on_click_listener(noop_click, v); // 借用：ctx = this
    delete v;
    EXPECT_EQ(CountingView::destroyed, 1);
}

// 拥有语义：deleter 非空时析构恰好释放一次。
TEST(ViewCallbackOwnership, OwnedCtxFreedOnceOnDestruction) {
    OwnedCtx::freed = 0;
    {
        UI::ViewGroup v(0, 0, 10, 10);
        v.set_on_click_listener(noop_click, new OwnedCtx{1}, &OwnedCtx::destroy);
        EXPECT_EQ(OwnedCtx::freed, 0);
    }
    EXPECT_EQ(OwnedCtx::freed, 1);
}

// clear_on_click_listener() 释放 owned ctx，且与析构不重复释放（release 幂等）。
TEST(ViewCallbackOwnership, ClearReleasesOwnedCtxOnce) {
    OwnedCtx::freed = 0;
    UI::ViewGroup v(0, 0, 10, 10);
    v.set_on_click_listener(noop_click, new OwnedCtx{1}, &OwnedCtx::destroy);
    v.clear_on_click_listener();
    EXPECT_EQ(OwnedCtx::freed, 1);
    v.clear_on_click_listener(); // 幂等：空槽再清无副作用
    EXPECT_EQ(OwnedCtx::freed, 1);
}

// 覆盖旧值：重复 set 同一槽时旧 owned ctx 立即释放（不泄漏）。
TEST(ViewCallbackOwnership, ResetReleasesPreviousOwnedCtx) {
    OwnedCtx::freed = 0;
    UI::ViewGroup v(0, 0, 10, 10);
    v.set_on_click_listener(noop_click, new OwnedCtx{1}, &OwnedCtx::destroy);
    v.set_on_click_listener(noop_click, new OwnedCtx{2}, &OwnedCtx::destroy);
    EXPECT_EQ(OwnedCtx::freed, 1); // 旧的已随覆盖释放
    v.set_on_click_listener(noop_click, new OwnedCtx{3}, &OwnedCtx::destroy);
    EXPECT_EQ(OwnedCtx::freed, 2);
}

// 4 个槽相互独立，析构释放全部 4 个 owned ctx。
// 旧版只释放 click，long/double/touch 的 ctx 泄漏（此断言旧代码 = 0，必 FAIL）。
TEST(ViewCallbackOwnership, AllFourSlotsReleasedOnDestruction) {
    OwnedCtx::freed = 0;
    {
        UI::ViewGroup v(0, 0, 10, 10);
        v.set_on_click_listener(noop_click, new OwnedCtx{1}, &OwnedCtx::destroy);
        v.set_on_long_click_listener(noop_long, new OwnedCtx{2}, &OwnedCtx::destroy);
        v.set_on_double_click_listener(noop_double, new OwnedCtx{3}, &OwnedCtx::destroy);
        v.set_on_touch_listener(noop_touch, new OwnedCtx{4}, &OwnedCtx::destroy);
        EXPECT_EQ(OwnedCtx::freed, 0);
    }
    EXPECT_EQ(OwnedCtx::freed, 4);
}

// 借用与拥有混用：仅 click 拥有、其余借用，析构只释放 click 那一个。
TEST(ViewCallbackOwnership, MixedBorrowedAndOwnedSlots) {
    OwnedCtx::freed = 0;
    {
        UI::ViewGroup v(0, 0, 10, 10);
        int borrowed = 0;
        v.set_on_click_listener(noop_click, new OwnedCtx{1}, &OwnedCtx::destroy); // owned
        v.set_on_long_click_listener(noop_long, &borrowed);                       // borrowed
        v.set_on_double_click_listener(noop_double, &borrowed);                   // borrowed
        v.set_on_touch_listener(noop_touch, &borrowed);                           // borrowed
    }
    EXPECT_EQ(OwnedCtx::freed, 1);
}

// 访问器改名后回调分发仍正常（handle_gesture 走新的 ListenerSlot）。
TEST(ViewCallbackOwnership, DispatchStillWorksAfterRefactor) {
    UI::ViewGroup v(0, 0, 100, 100);
    int clicks = 0;
    v.set_on_click_listener([](UI::View*, void* ctx) { ++*static_cast<int*>(ctx); }, &clicks);

    GestureEvent tap = {GestureType::TAP, 10, 10};
    EXPECT_TRUE(v.handle_gesture(tap));
    EXPECT_EQ(clicks, 1);
}

// =============================================================================
// Renderer2D::ClipScope —— 裁剪区栈式保存/恢复（RAII）
// 覆盖额外缺陷 1：用 set_clip_rect（求交语义）「恢复」会把裁剪区永久收窄。
// =============================================================================

// 嵌套 ClipScope：内层析构恢复到外层值，外层析构恢复到最初值。
TEST(ClipScope, NestedRestoresExactClip) {
    UI::UIRenderer r(s_fb);
    r.clear_clip_rect();
    r.set_clip_rect(0, 0, 50, 50);
    EXPECT_EQ(r.get_clip_rect().w, 50);

    {
        UI::UIRenderer::ClipScope outer(r);
        r.set_clip_rect(10, 10, 20, 20);
        EXPECT_EQ(r.get_clip_rect().w, 20);
        {
            UI::UIRenderer::ClipScope inner(r);
            r.set_clip_rect(12, 12, 5, 5);
            EXPECT_EQ(r.get_clip_rect().w, 5);
        }
        EXPECT_EQ(r.get_clip_rect().w, 20); // 内层析构 → 恢复到外层写入前的 20
    }
    EXPECT_EQ(r.get_clip_rect().w, 50); // 外层析构 → 恢复到最初的 50
}

// 提前 return 路径亦还原（RAII 保证，不依赖手写恢复语句到达）。
TEST(ClipScope, RestoresOnEarlyReturn) {
    UI::UIRenderer r(s_fb);
    r.clear_clip_rect();
    const Rect2D before = r.get_clip_rect();

    draw_and_early_return(r);

    const Rect2D after = r.get_clip_rect();
    EXPECT_EQ(after.x, before.x);
    EXPECT_EQ(after.y, before.y);
    EXPECT_EQ(after.w, before.w);
    EXPECT_EQ(after.h, before.h);
}

// ScrollView::draw 结束后必须把裁剪区还原为入栈时的全屏值，
// 而非永久收窄到自身矩形（额外缺陷 1 回归；旧代码此处 = (0,0,100,20)）。
TEST(ClipScope, ScrollViewRestoresSiblingClip) {
    UI::UIRenderer r(s_fb);
    r.clear_clip_rect();

    UI::ScrollView sv(0, 0, 100, 20);
    sv.draw(r);

    const Rect2D after = r.get_clip_rect();
    EXPECT_EQ(after.x, 0);
    EXPECT_EQ(after.y, 0);
    EXPECT_EQ(after.w, DISPLAY_WIDTH);
    EXPECT_EQ(after.h, AURORA_FB_CHUNK_HEIGHT);
}
