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

#include "../../ui/view.hpp"
#include "../../ui/view_group.hpp"

namespace {

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
