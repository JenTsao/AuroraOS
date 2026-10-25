#ifndef AURORA_JS_UI_BINDING_HPP
#define AURORA_JS_UI_BINDING_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../kernel/mm/memory.hpp"
#include "../ui/screen.hpp"
#include "../ui/screen_navigator.hpp"
#include "../ui/ui_manager.hpp"
#include "../ui/view_group.hpp"
#include "../ui/widgets/arc_progress.hpp"
#include "../ui/widgets/button.hpp"
#include "../ui/widgets/progress_bar.hpp"
#include "../ui/widgets/scroll_view.hpp"
#include "../ui/widgets/slider_view.hpp"
#include "../ui/widgets/switch_view.hpp"
#include "../ui/widgets/text_view.hpp"

extern "C" {
#include "../3rdparty/quickjs/quickjs.h"
}

// =============================================================================
// QuickJS UI 绑定契约（对齐 apps/lua_ui_binding.hpp/.cpp 并扩展完整控件族）
//
// 所有权模型（JsViewUserData）：
//   1. attached == false（默认）：
//      视图尚未挂入宿主 C++ 视图树，归 QuickJS wrapper 对象持有。
//      当脚本丢弃引用或引擎销毁时，由 QuickJS finalizer (js_view_finalizer)
//      负责 delete 底层 UI::View，防止孤儿视图泄漏内核堆。
//   2. attached == true：
//      视图已通过 add_child / set_root_view / navigator_push / create_ui
//      移交宿主 C++ 视图树，由宿主 ViewGroup 析构链统一释放；
//      js_view_finalizer 仅释放 JS 侧轻量元数据（JsViewUserData），绝不重复
//      delete 底层 UI::View。
//   3. 防重挂载守卫：
//      已被移交（attached == true）的视图禁止再次 add_child / set_root_view /
//      navigator_push，从入口杜绝同一 C++ View* 进入两处容器造成的双重释放。
// =============================================================================

enum class JsViewKind : uint8_t {
    VIEW_GROUP = 0,
    TEXT_VIEW = 1,
    ARC_PROGRESS = 2,
    BUTTON = 3,
    PROGRESS_BAR = 4,
    SWITCH_VIEW = 5,
    SLIDER_VIEW = 6,
    SCROLL_VIEW = 7
};

struct JsViewUserData {
    UI::View* view;
    bool attached;
    JsViewKind kind;
};

// 宿主执行控制接口：由 JsMiniProgramEngine 实现并挂在 JS_SetContextOpaque(ctx)，
// 使 UI 异步事件回调（点击/滑动/开关）同样受指令预算与连续异常熔断器保护。
class IJsExecutionHost {
public:
    virtual ~IJsExecutionHost() = default;
    virtual bool can_execute_callback() const noexcept = 0;
    virtual void on_before_callback() noexcept = 0;
    virtual void on_callback_result(bool success) noexcept = 0;
};

namespace aurora {
namespace jsui {

// 自持文本缓冲的 TextView 派生类：
// QuickJS 的 JS_ToCString 返回临时引用，JS_FreeCString 后若原 JSString 为临时拼接
// 字符串即刻被释放。UI::TextView 仅保存裸指针 const char*，故在 JS 绑定层内联
// 64B 缓冲区持久化文本，彻底消除悬空指针（use-after-free）与额外堆分配。
class JsTextView : public UI::TextView {
public:
    static constexpr size_t kMaxOwnedTextLen = 63;

    JsTextView(int16_t x, int16_t y, const char* text, ColorRGB565 fg, ColorRGB565 bg = 0, uint8_t scale = 2,
               UI::TextAlign align = UI::TextAlign::LEFT)
        : UI::TextView(x, y, nullptr, fg, bg, scale, align) {
        set_owned_text(text);
    }

    void set_owned_text(const char* text) noexcept {
        if (!text) {
            owned_text_[0] = '\0';
        } else {
            size_t i = 0;
            while (text[i] != '\0' && i < kMaxOwnedTextLen) {
                owned_text_[i] = text[i];
                ++i;
            }
            owned_text_[i] = '\0';
        }
        UI::TextView::set_text(owned_text_);
    }

private:
    char owned_text_[kMaxOwnedTextLen + 1]{};
};

class JsScreen : public UI::Screen {
public:
    explicit JsScreen(UI::ViewGroup* root) : root_(root) {
        add_child(root_);
    }

private:
    UI::ViewGroup* root_;
};

// =============================================================================
// 回调上下文（JsCallbackCtx）与双向生命周期解耦
//
// 为什么需要侵入式链表 + CtxGuard：
//   - 若 C++ View 先于 JSContext 销毁（如 ScreenNavigator::clear 或覆盖监听），
//     View::~View() -> ListenerSlot::release() 调用 js_ctx_release 释放 JSValue。
//   - 若 JSContext 先于 C++ View 销毁（如 DynamicJsWatchFaceScreen 先析构成员
//     engine_ 再析构基类 ViewGroup，或测试中先关 VM 再清 UiManager），
//     QuickJS 的 JS_FreeRuntime 要求所有 GC 对象引用计数归零（assert(list_empty)）。
//     此时绑定在 ctx->class_proto 上的 CtxGuard 在 JS_FreeContext 阶段即刻触发
//     finalizer，遍历链表提前释放该 ctx 下所有 JSValue 并将 c->rt 置空；
//     随后 View 析构时 js_ctx_release 见 c->rt == nullptr 仅释放 C++ 结构体本身。
// =============================================================================
struct JsCallbackCtx {
    JSContext* ctx;
    JSRuntime* rt;
    UI::View* owner_view;
    JSValue fn;
    JsCallbackCtx* prev;
    JsCallbackCtx* next;
};

inline JsCallbackCtx*& callback_list_head() noexcept {
    static JsCallbackCtx* head = nullptr;
    return head;
}

inline void link_callback_ctx(JsCallbackCtx* c) noexcept {
    JsCallbackCtx*& head = callback_list_head();
    c->prev = nullptr;
    c->next = head;
    if (head) {
        head->prev = c;
    }
    head = c;
}

inline void unlink_callback_ctx(JsCallbackCtx* c) noexcept {
    JsCallbackCtx*& head = callback_list_head();
    if (c->prev) {
        c->prev->next = c->next;
    } else if (head == c) {
        head = c->next;
    }
    if (c->next) {
        c->next->prev = c->prev;
    }
    c->prev = nullptr;
    c->next = nullptr;
}

inline void detach_callbacks_for_context(JSContext* dying_ctx) noexcept {
    JsCallbackCtx* cur = callback_list_head();
    while (cur) {
        JsCallbackCtx* next = cur->next;
        if (cur->ctx == dying_ctx && cur->rt != nullptr) {
            unlink_callback_ctx(cur);
            JSRuntime* rt = cur->rt;
            JSValue fn = cur->fn;
            cur->ctx = nullptr;
            cur->rt = nullptr;
            cur->fn = JS_UNDEFINED;
            JS_FreeValueRT(rt, fn);
        }
        cur = next;
    }
}

inline void js_ctx_release(void* p) noexcept {
    auto* c = static_cast<JsCallbackCtx*>(p);
    if (!c) {
        return;
    }
    if (c->rt != nullptr) {
        unlink_callback_ctx(c);
        JSRuntime* rt = c->rt;
        JSValue fn = c->fn;
        c->ctx = nullptr;
        c->rt = nullptr;
        c->fn = JS_UNDEFINED;
        JS_FreeValueRT(rt, fn);
    }
    delete c;
}

inline void invoke_js_callback(JsCallbackCtx* c, int argc, JSValueConst* argv) noexcept {
    if (!c || !c->ctx || !c->rt || JS_IsUndefined(c->fn)) {
        return;
    }
    JSContext* ctx = c->ctx;
    auto* host = static_cast<IJsExecutionHost*>(JS_GetContextOpaque(ctx));
    if (host) {
        if (!host->can_execute_callback()) {
            return;
        }
        host->on_before_callback();
    }

    JSValue ret = JS_Call(ctx, c->fn, JS_UNDEFINED, argc, argv);
    if (JS_IsException(ret)) {
        JSValue ex = JS_GetException(ctx);
        JS_FreeValue(ctx, ex);
        if (host) {
            host->on_callback_result(false);
        }
    } else if (host) {
        host->on_callback_result(true);
    }
    JS_FreeValue(ctx, ret);
}

inline void js_view_on_click(UI::View* /*v*/, void* ctx) {
    invoke_js_callback(static_cast<JsCallbackCtx*>(ctx), 0, nullptr);
}

inline void js_switch_on_changed(UI::SwitchView* /*sw*/, bool checked, void* ctx) {
    auto* c = static_cast<JsCallbackCtx*>(ctx);
    if (!c || !c->ctx) {
        return;
    }
    JSValue arg = JS_NewBool(c->ctx, checked ? 1 : 0);
    invoke_js_callback(c, 1, &arg);
    JS_FreeValue(c->ctx, arg);
}

inline void js_slider_on_changed(UI::SliderView* /*slider*/, int32_t value, void* ctx) {
    auto* c = static_cast<JsCallbackCtx*>(ctx);
    if (!c || !c->ctx) {
        return;
    }
    JSValue arg = JS_NewInt32(c->ctx, value);
    invoke_js_callback(c, 1, &arg);
    JS_FreeValue(c->ctx, arg);
}

inline JsCallbackCtx* create_callback_ctx(JSContext* ctx, UI::View* view, JSValueConst fn_val) {
    auto* c = new JsCallbackCtx{ctx, JS_GetRuntime(ctx), view, JS_DupValue(ctx, fn_val), nullptr, nullptr};
    link_callback_ctx(c);
    return c;
}

// =============================================================================
// QuickJS Class IDs & Finalizers
// =============================================================================
inline JSClassID js_view_class_id() noexcept {
    static JSClassID id = 0;
    if (id == 0) {
        JS_NewClassID(&id);
    }
    return id;
}

inline JSClassID js_ctx_guard_class_id() noexcept {
    static JSClassID id = 0;
    if (id == 0) {
        JS_NewClassID(&id);
    }
    return id;
}

inline void js_view_finalizer(JSRuntime* rt, JSValue val) {
    auto* ud = static_cast<JsViewUserData*>(JS_GetOpaque(val, js_view_class_id()));
    if (ud != nullptr) {
        JS_SetOpaque(val, nullptr);
        if (!ud->attached && ud->view != nullptr) {
            delete ud->view;
            ud->view = nullptr;
        }
        js_free_rt(rt, ud);
    }
}

// 孤儿视图若注册了捕获自身的闭包回调（如 tv.set_on_click_listener(() => tv.set_text(...))），
// 通过 gc_mark 向 QuickJS 声明「未挂载视图持有其回调 JSValue」，使环路收集器能够识别并打破
// [JS View wrapper -> JsCallbackCtx::fn -> JS View wrapper] 循环引用。
// 已挂载视图（attached == true）归 C++ 宿主树持有，不参与孤儿环路回收。
inline void js_view_gc_mark(JSRuntime* rt, JSValueConst val, JS_MarkFunc* mark_func) {
    auto* ud = static_cast<JsViewUserData*>(JS_GetOpaque(val, js_view_class_id()));
    if (!ud || ud->attached || !ud->view) {
        return;
    }
    for (JsCallbackCtx* cur = callback_list_head(); cur != nullptr; cur = cur->next) {
        if (cur->rt == rt && cur->owner_view == ud->view && !JS_IsUndefined(cur->fn)) {
            JS_MarkValue(rt, cur->fn, mark_func);
        }
    }
}

inline void js_ctx_guard_finalizer(JSRuntime* /*rt*/, JSValue val) {
    auto* dying_ctx = static_cast<JSContext*>(JS_GetOpaque(val, js_ctx_guard_class_id()));
    if (dying_ctx != nullptr) {
        JS_SetOpaque(val, nullptr);
        detach_callbacks_for_context(dying_ctx);
    }
}

constexpr bool is_view_group_kind(JsViewKind kind) noexcept {
    return kind == JsViewKind::VIEW_GROUP || kind == JsViewKind::BUTTON || kind == JsViewKind::SCROLL_VIEW;
}

inline JsViewUserData* check_view_ud(JSContext* ctx, JSValueConst val) {
    auto* ud = static_cast<JsViewUserData*>(JS_GetOpaque2(ctx, val, js_view_class_id()));
    if (!ud || !ud->view) {
        return nullptr;
    }
    return ud;
}

inline void ensure_view_class_proto(JSContext* ctx);

inline JSValue wrap_view(JSContext* ctx, UI::View* view, JsViewKind kind) {
    if (!view) {
        return JS_ThrowOutOfMemory(ctx);
    }
    ensure_view_class_proto(ctx);
    auto* ud = static_cast<JsViewUserData*>(js_malloc(ctx, sizeof(JsViewUserData)));
    if (!ud) {
        delete view;
        return JS_ThrowOutOfMemory(ctx);
    }
    ud->view = view;
    ud->attached = false;
    ud->kind = kind;

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_view_class_id()));
    if (JS_IsException(obj)) {
        js_free(ctx, ud);
        delete view;
        return obj;
    }
    JS_SetOpaque(obj, ud);
    return obj;
}

// =============================================================================
// 控件构造函数绑定（aurora.ui.*）
// =============================================================================
inline JSValue js_view_group_new(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 4) {
        return JS_ThrowTypeError(ctx, "ViewGroup(x, y, w, h) requires 4 arguments");
    }
    int32_t x = 0, y = 0, w = 0, h = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]) || JS_ToInt32(ctx, &w, argv[2]) ||
        JS_ToInt32(ctx, &h, argv[3])) {
        return JS_EXCEPTION;
    }
    if (w < 0 || h < 0) {
        return JS_ThrowRangeError(ctx, "ViewGroup dimensions must be non-negative");
    }
    auto* vg = new UI::ViewGroup(static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(w),
                                 static_cast<uint16_t>(h));
    return wrap_view(ctx, vg, JsViewKind::VIEW_GROUP);
}

inline JSValue js_text_view_new(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 4) {
        return JS_ThrowTypeError(ctx, "TextView(x, y, text, fg[, bg, scale, align]) requires at least 4 arguments");
    }
    int32_t x = 0, y = 0, fg = 0xFFFF, bg = 0, scale = 2, align = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]) || JS_ToInt32(ctx, &fg, argv[3])) {
        return JS_EXCEPTION;
    }
    if (argc >= 5 && JS_ToInt32(ctx, &bg, argv[4])) {
        return JS_EXCEPTION;
    }
    if (argc >= 6 && JS_ToInt32(ctx, &scale, argv[5])) {
        return JS_EXCEPTION;
    }
    if (argc >= 7 && JS_ToInt32(ctx, &align, argv[6])) {
        return JS_EXCEPTION;
    }
    if (scale <= 0) {
        scale = 1;
    }
    const char* text = JS_ToCString(ctx, argv[2]);
    if (!text) {
        return JS_EXCEPTION;
    }
    UI::TextAlign text_align = UI::TextAlign::LEFT;
    if (align == 1) {
        text_align = UI::TextAlign::CENTER;
    } else if (align == 2) {
        text_align = UI::TextAlign::RIGHT;
    }
    auto* tv = new JsTextView(static_cast<int16_t>(x), static_cast<int16_t>(y), text, static_cast<ColorRGB565>(fg),
                              static_cast<ColorRGB565>(bg), static_cast<uint8_t>(scale), text_align);
    JS_FreeCString(ctx, text);
    return wrap_view(ctx, tv, JsViewKind::TEXT_VIEW);
}

inline JSValue js_arc_progress_new(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 5) {
        return JS_ThrowTypeError(ctx, "ArcProgress(cx, cy, r, val, color) requires 5 arguments");
    }
    int32_t cx = 0, cy = 0, r = 0, val = 0, color = 0;
    if (JS_ToInt32(ctx, &cx, argv[0]) || JS_ToInt32(ctx, &cy, argv[1]) || JS_ToInt32(ctx, &r, argv[2]) ||
        JS_ToInt32(ctx, &val, argv[3]) || JS_ToInt32(ctx, &color, argv[4])) {
        return JS_EXCEPTION;
    }
    if (r < 0) {
        return JS_ThrowRangeError(ctx, "ArcProgress radius must be non-negative");
    }
    if (val < 0) {
        val = 0;
    } else if (val > 100) {
        val = 100;
    }
    auto* arc = new UI::ArcProgress(static_cast<int16_t>(cx), static_cast<int16_t>(cy), static_cast<uint16_t>(r),
                                    static_cast<uint8_t>(val), static_cast<ColorRGB565>(color));
    return wrap_view(ctx, arc, JsViewKind::ARC_PROGRESS);
}

inline JSValue js_button_new(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 6) {
        return JS_ThrowTypeError(ctx, "Button(x, y, w, h, bg, pressed[, radius]) requires at least 6 arguments");
    }
    int32_t x = 0, y = 0, w = 0, h = 0, bg = 0, pressed = 0, radius = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]) || JS_ToInt32(ctx, &w, argv[2]) ||
        JS_ToInt32(ctx, &h, argv[3]) || JS_ToInt32(ctx, &bg, argv[4]) || JS_ToInt32(ctx, &pressed, argv[5])) {
        return JS_EXCEPTION;
    }
    if (argc >= 7 && JS_ToInt32(ctx, &radius, argv[6])) {
        return JS_EXCEPTION;
    }
    if (w < 0 || h < 0 || radius < 0) {
        return JS_ThrowRangeError(ctx, "Button dimensions and radius must be non-negative");
    }
    auto* btn = new UI::Button(static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(w),
                               static_cast<uint16_t>(h), static_cast<ColorRGB565>(bg),
                               static_cast<ColorRGB565>(pressed), static_cast<uint16_t>(radius));
    return wrap_view(ctx, btn, JsViewKind::BUTTON);
}

inline JSValue js_progress_bar_new(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 4) {
        return JS_ThrowTypeError(
            ctx, "ProgressBar(x, y, w, h[, min, max, cur, bg, fill, radius]) requires at least 4 arguments");
    }
    int32_t x = 0, y = 0, w = 0, h = 0;
    int32_t min_v = 0, max_v = 100, cur_v = 0;
    int32_t bg = 0x18C3, fill = 0x001F, radius = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]) || JS_ToInt32(ctx, &w, argv[2]) ||
        JS_ToInt32(ctx, &h, argv[3])) {
        return JS_EXCEPTION;
    }
    if (argc >= 5 && JS_ToInt32(ctx, &min_v, argv[4])) {
        return JS_EXCEPTION;
    }
    if (argc >= 6 && JS_ToInt32(ctx, &max_v, argv[5])) {
        return JS_EXCEPTION;
    }
    if (argc >= 7 && JS_ToInt32(ctx, &cur_v, argv[6])) {
        return JS_EXCEPTION;
    }
    if (argc >= 8 && JS_ToInt32(ctx, &bg, argv[7])) {
        return JS_EXCEPTION;
    }
    if (argc >= 9 && JS_ToInt32(ctx, &fill, argv[8])) {
        return JS_EXCEPTION;
    }
    if (argc >= 10 && JS_ToInt32(ctx, &radius, argv[9])) {
        return JS_EXCEPTION;
    }
    if (w < 0 || h < 0 || radius < 0) {
        return JS_ThrowRangeError(ctx, "ProgressBar dimensions and radius must be non-negative");
    }
    auto* bar = new UI::ProgressBar(static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(w),
                                    static_cast<uint16_t>(h), min_v, max_v, cur_v, static_cast<ColorRGB565>(bg),
                                    static_cast<ColorRGB565>(fill), static_cast<uint16_t>(radius));
    return wrap_view(ctx, bar, JsViewKind::PROGRESS_BAR);
}

inline JSValue js_switch_view_new(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(
            ctx, "SwitchView(x, y[, w, h, checked, onColor, offColor, thumbColor]) requires at least 2 arguments");
    }
    int32_t x = 0, y = 0, w = 50, h = 28;
    bool checked = false;
    int32_t on_color = 0x07E0, off_color = 0x39E7, thumb_color = 0xFFFF;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1])) {
        return JS_EXCEPTION;
    }
    if (argc >= 3 && JS_ToInt32(ctx, &w, argv[2])) {
        return JS_EXCEPTION;
    }
    if (argc >= 4 && JS_ToInt32(ctx, &h, argv[3])) {
        return JS_EXCEPTION;
    }
    if (argc >= 5) {
        int b = JS_ToBool(ctx, argv[4]);
        if (b < 0) {
            return JS_EXCEPTION;
        }
        checked = (b != 0);
    }
    if (argc >= 6 && JS_ToInt32(ctx, &on_color, argv[5])) {
        return JS_EXCEPTION;
    }
    if (argc >= 7 && JS_ToInt32(ctx, &off_color, argv[6])) {
        return JS_EXCEPTION;
    }
    if (argc >= 8 && JS_ToInt32(ctx, &thumb_color, argv[7])) {
        return JS_EXCEPTION;
    }
    if (w < 0 || h < 0) {
        return JS_ThrowRangeError(ctx, "SwitchView dimensions must be non-negative");
    }
    auto* sw = new UI::SwitchView(static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(w),
                                  static_cast<uint16_t>(h), checked, static_cast<ColorRGB565>(on_color),
                                  static_cast<ColorRGB565>(off_color), static_cast<ColorRGB565>(thumb_color));
    return wrap_view(ctx, sw, JsViewKind::SWITCH_VIEW);
}

inline JSValue js_slider_view_new(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 4) {
        return JS_ThrowTypeError(
            ctx,
            "SliderView(x, y, w, h[, min, max, cur, trackColor, fillColor, thumbColor]) requires at least 4 arguments");
    }
    int32_t x = 0, y = 0, w = 0, h = 0;
    int32_t min_v = 0, max_v = 100, cur_v = 50;
    int32_t track = 0x39E7, fill = 0x07E0, thumb = 0xFFFF;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]) || JS_ToInt32(ctx, &w, argv[2]) ||
        JS_ToInt32(ctx, &h, argv[3])) {
        return JS_EXCEPTION;
    }
    if (argc >= 5 && JS_ToInt32(ctx, &min_v, argv[4])) {
        return JS_EXCEPTION;
    }
    if (argc >= 6 && JS_ToInt32(ctx, &max_v, argv[5])) {
        return JS_EXCEPTION;
    }
    if (argc >= 7 && JS_ToInt32(ctx, &cur_v, argv[6])) {
        return JS_EXCEPTION;
    }
    if (argc >= 8 && JS_ToInt32(ctx, &track, argv[7])) {
        return JS_EXCEPTION;
    }
    if (argc >= 9 && JS_ToInt32(ctx, &fill, argv[8])) {
        return JS_EXCEPTION;
    }
    if (argc >= 10 && JS_ToInt32(ctx, &thumb, argv[9])) {
        return JS_EXCEPTION;
    }
    if (w < 0 || h < 0) {
        return JS_ThrowRangeError(ctx, "SliderView dimensions must be non-negative");
    }
    auto* slider = new UI::SliderView(static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(w),
                                      static_cast<uint16_t>(h), min_v, max_v, cur_v, static_cast<ColorRGB565>(track),
                                      static_cast<ColorRGB565>(fill), static_cast<ColorRGB565>(thumb));
    return wrap_view(ctx, slider, JsViewKind::SLIDER_VIEW);
}

inline JSValue js_scroll_view_new(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 4) {
        return JS_ThrowTypeError(ctx, "ScrollView(x, y, w, h) requires 4 arguments");
    }
    int32_t x = 0, y = 0, w = 0, h = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1]) || JS_ToInt32(ctx, &w, argv[2]) ||
        JS_ToInt32(ctx, &h, argv[3])) {
        return JS_EXCEPTION;
    }
    if (w < 0 || h < 0) {
        return JS_ThrowRangeError(ctx, "ScrollView dimensions must be non-negative");
    }
    auto* sv = new UI::ScrollView(static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<uint16_t>(w),
                                  static_cast<uint16_t>(h));
    return wrap_view(ctx, sv, JsViewKind::SCROLL_VIEW);
}

// =============================================================================
// View 原型方法绑定
// =============================================================================
inline JSValue js_view_add_child(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* parent_ud = check_view_ud(ctx, this_val);
    if (!parent_ud) {
        return JS_EXCEPTION;
    }
    if (!is_view_group_kind(parent_ud->kind)) {
        return JS_ThrowTypeError(ctx, "add_child requires a ViewGroup container");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "add_child expects a child View");
    }
    JsViewUserData* child_ud = check_view_ud(ctx, argv[0]);
    if (!child_ud) {
        return JS_EXCEPTION;
    }
    if (child_ud->view == parent_ud->view) {
        return JS_ThrowTypeError(ctx, "cannot attach a ViewGroup to itself");
    }
    if (child_ud->attached) {
        return JS_ThrowTypeError(ctx, "child View is already attached to a host hierarchy");
    }

    UI::View* child = child_ud->view;
    if (parent_ud->kind == JsViewKind::SCROLL_VIEW) {
        static_cast<UI::ScrollView*>(parent_ud->view)->add_child(child);
    } else {
        static_cast<UI::ViewGroup*>(parent_ud->view)->add_child(child);
    }

    // 仅当容器真正接纳该子视图（未超出 MAX_CHILDREN）时移交所有权；
    // 被拒收的视图仍归 JS 侧持有，由 finalizer 回收。
    if (child->get_parent() == parent_ud->view) {
        child_ud->attached = true;
    }
    return JS_UNDEFINED;
}

inline JSValue js_view_get_child_count(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (!is_view_group_kind(ud->kind)) {
        return JS_ThrowTypeError(ctx, "get_child_count requires a ViewGroup container");
    }
    return JS_NewInt32(ctx, static_cast<UI::ViewGroup*>(ud->view)->get_child_count());
}

inline JSValue js_text_view_set_text(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::TEXT_VIEW) {
        return JS_ThrowTypeError(ctx, "set_text requires a TextView");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_text expects a string argument");
    }
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) {
        return JS_EXCEPTION;
    }
    static_cast<JsTextView*>(ud->view)->set_owned_text(text);
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

inline JSValue js_text_view_get_text(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::TEXT_VIEW) {
        return JS_ThrowTypeError(ctx, "get_text requires a TextView");
    }
    const char* text = static_cast<JsTextView*>(ud->view)->get_text();
    return JS_NewString(ctx, text ? text : "");
}

inline JSValue js_text_view_set_text_color(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::TEXT_VIEW) {
        return JS_ThrowTypeError(ctx, "set_text_color requires a TextView");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_text_color(fg[, bg]) requires at least 1 argument");
    }
    int32_t fg = 0, bg = 0;
    if (JS_ToInt32(ctx, &fg, argv[0])) {
        return JS_EXCEPTION;
    }
    if (argc >= 2 && JS_ToInt32(ctx, &bg, argv[1])) {
        return JS_EXCEPTION;
    }
    static_cast<JsTextView*>(ud->view)->set_text_color(static_cast<ColorRGB565>(fg), static_cast<ColorRGB565>(bg));
    return JS_UNDEFINED;
}

inline JSValue js_text_view_set_text_align(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::TEXT_VIEW) {
        return JS_ThrowTypeError(ctx, "set_text_align requires a TextView");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_text_align expects an alignment constant");
    }
    int32_t align = 0;
    if (JS_ToInt32(ctx, &align, argv[0])) {
        return JS_EXCEPTION;
    }
    UI::TextAlign a = UI::TextAlign::LEFT;
    if (align == 1) {
        a = UI::TextAlign::CENTER;
    } else if (align == 2) {
        a = UI::TextAlign::RIGHT;
    }
    static_cast<JsTextView*>(ud->view)->set_text_align(a);
    return JS_UNDEFINED;
}

inline JSValue js_text_view_set_scale(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::TEXT_VIEW) {
        return JS_ThrowTypeError(ctx, "set_scale requires a TextView");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_scale expects a positive integer");
    }
    int32_t scale = 1;
    if (JS_ToInt32(ctx, &scale, argv[0])) {
        return JS_EXCEPTION;
    }
    if (scale > 0) {
        static_cast<JsTextView*>(ud->view)->set_scale(static_cast<uint8_t>(scale));
    }
    return JS_UNDEFINED;
}

inline JSValue js_arc_progress_set_percentage(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::ARC_PROGRESS) {
        return JS_ThrowTypeError(ctx, "set_percentage requires an ArcProgress");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_percentage expects an integer percentage");
    }
    int32_t pct = 0;
    if (JS_ToInt32(ctx, &pct, argv[0])) {
        return JS_EXCEPTION;
    }
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    static_cast<UI::ArcProgress*>(ud->view)->set_percentage(static_cast<uint8_t>(pct));
    return JS_UNDEFINED;
}

inline JSValue js_progress_bar_set_progress(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::PROGRESS_BAR) {
        return JS_ThrowTypeError(ctx, "set_progress requires a ProgressBar");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_progress expects an integer value");
    }
    int32_t val = 0;
    if (JS_ToInt32(ctx, &val, argv[0])) {
        return JS_EXCEPTION;
    }
    static_cast<UI::ProgressBar*>(ud->view)->set_progress(val);
    return JS_UNDEFINED;
}

inline JSValue js_progress_bar_get_progress(JSContext* ctx, JSValueConst this_val, int /*argc*/,
                                            JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::PROGRESS_BAR) {
        return JS_ThrowTypeError(ctx, "get_progress requires a ProgressBar");
    }
    return JS_NewInt32(ctx, static_cast<UI::ProgressBar*>(ud->view)->get_progress());
}

inline JSValue js_progress_bar_get_percentage(JSContext* ctx, JSValueConst this_val, int /*argc*/,
                                              JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::PROGRESS_BAR) {
        return JS_ThrowTypeError(ctx, "get_percentage requires a ProgressBar");
    }
    return JS_NewInt32(ctx, static_cast<UI::ProgressBar*>(ud->view)->get_percentage());
}

inline JSValue js_view_set_range(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "set_range(min, max) requires 2 arguments");
    }
    int32_t min_v = 0, max_v = 100;
    if (JS_ToInt32(ctx, &min_v, argv[0]) || JS_ToInt32(ctx, &max_v, argv[1])) {
        return JS_EXCEPTION;
    }
    if (ud->kind == JsViewKind::PROGRESS_BAR) {
        static_cast<UI::ProgressBar*>(ud->view)->set_range(min_v, max_v);
    } else if (ud->kind == JsViewKind::SLIDER_VIEW) {
        static_cast<UI::SliderView*>(ud->view)->set_range(min_v, max_v);
    } else {
        return JS_ThrowTypeError(ctx, "set_range requires a ProgressBar or SliderView");
    }
    return JS_UNDEFINED;
}

inline JSValue js_view_set_corner_radius(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_corner_radius(r) requires 1 argument");
    }
    int32_t r = 0;
    if (JS_ToInt32(ctx, &r, argv[0])) {
        return JS_EXCEPTION;
    }
    if (r < 0) {
        return JS_ThrowRangeError(ctx, "corner radius must be non-negative");
    }
    if (ud->kind == JsViewKind::BUTTON) {
        static_cast<UI::Button*>(ud->view)->set_corner_radius(static_cast<uint16_t>(r));
    } else if (ud->kind == JsViewKind::PROGRESS_BAR) {
        static_cast<UI::ProgressBar*>(ud->view)->set_corner_radius(static_cast<uint16_t>(r));
    } else {
        return JS_ThrowTypeError(ctx, "set_corner_radius requires a Button or ProgressBar");
    }
    return JS_UNDEFINED;
}

inline JSValue js_switch_set_checked(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SWITCH_VIEW) {
        return JS_ThrowTypeError(ctx, "set_checked requires a SwitchView");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_checked expects a boolean argument");
    }
    int b = JS_ToBool(ctx, argv[0]);
    if (b < 0) {
        return JS_EXCEPTION;
    }
    bool trigger = true;
    if (argc >= 2) {
        int t = JS_ToBool(ctx, argv[1]);
        if (t < 0) {
            return JS_EXCEPTION;
        }
        trigger = (t != 0);
    }
    static_cast<UI::SwitchView*>(ud->view)->set_checked(b != 0, trigger);
    return JS_UNDEFINED;
}

inline JSValue js_switch_is_checked(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SWITCH_VIEW) {
        return JS_ThrowTypeError(ctx, "is_checked requires a SwitchView");
    }
    return JS_NewBool(ctx, static_cast<UI::SwitchView*>(ud->view)->is_checked() ? 1 : 0);
}

inline JSValue js_switch_toggle(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SWITCH_VIEW) {
        return JS_ThrowTypeError(ctx, "toggle requires a SwitchView");
    }
    static_cast<UI::SwitchView*>(ud->view)->toggle();
    return JS_UNDEFINED;
}

inline JSValue js_switch_set_on_checked_changed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SWITCH_VIEW) {
        return JS_ThrowTypeError(ctx, "set_on_checked_changed_listener requires a SwitchView");
    }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Expected function");
    }
    JsCallbackCtx* cb_ctx = create_callback_ctx(ctx, ud->view, argv[0]);
    static_cast<UI::SwitchView*>(ud->view)->set_on_checked_changed_listener(js_switch_on_changed, cb_ctx,
                                                                            &js_ctx_release);
    return JS_UNDEFINED;
}

inline JSValue js_slider_set_value(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SLIDER_VIEW) {
        return JS_ThrowTypeError(ctx, "set_value requires a SliderView");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_value expects an integer argument");
    }
    int32_t val = 0;
    if (JS_ToInt32(ctx, &val, argv[0])) {
        return JS_EXCEPTION;
    }
    static_cast<UI::SliderView*>(ud->view)->set_value(val);
    return JS_UNDEFINED;
}

inline JSValue js_slider_get_value(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SLIDER_VIEW) {
        return JS_ThrowTypeError(ctx, "get_value requires a SliderView");
    }
    return JS_NewInt32(ctx, static_cast<UI::SliderView*>(ud->view)->get_value());
}

inline JSValue js_slider_set_on_value_changed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SLIDER_VIEW) {
        return JS_ThrowTypeError(ctx, "set_on_value_changed_listener requires a SliderView");
    }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Expected function");
    }
    JsCallbackCtx* cb_ctx = create_callback_ctx(ctx, ud->view, argv[0]);
    static_cast<UI::SliderView*>(ud->view)->set_on_value_changed_listener(js_slider_on_changed, cb_ctx,
                                                                          &js_ctx_release);
    return JS_UNDEFINED;
}

inline JSValue js_scroll_view_scroll_to(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SCROLL_VIEW) {
        return JS_ThrowTypeError(ctx, "scroll_to requires a ScrollView");
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "scroll_to(x, y) requires 2 arguments");
    }
    int32_t sx = 0, sy = 0;
    if (JS_ToInt32(ctx, &sx, argv[0]) || JS_ToInt32(ctx, &sy, argv[1])) {
        return JS_EXCEPTION;
    }
    static_cast<UI::ScrollView*>(ud->view)->scroll_to(static_cast<int16_t>(sx), static_cast<int16_t>(sy));
    return JS_UNDEFINED;
}

inline JSValue js_scroll_view_scroll_by(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SCROLL_VIEW) {
        return JS_ThrowTypeError(ctx, "scroll_by requires a ScrollView");
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "scroll_by(dx, dy) requires 2 arguments");
    }
    int32_t dx = 0, dy = 0;
    if (JS_ToInt32(ctx, &dx, argv[0]) || JS_ToInt32(ctx, &dy, argv[1])) {
        return JS_EXCEPTION;
    }
    static_cast<UI::ScrollView*>(ud->view)->scroll_by(static_cast<int16_t>(dx), static_cast<int16_t>(dy));
    return JS_UNDEFINED;
}

inline JSValue js_scroll_view_set_content_size(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SCROLL_VIEW) {
        return JS_ThrowTypeError(ctx, "set_content_size requires a ScrollView");
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "set_content_size(w, h) requires 2 arguments");
    }
    int32_t w = 0, h = 0;
    if (JS_ToInt32(ctx, &w, argv[0]) || JS_ToInt32(ctx, &h, argv[1])) {
        return JS_EXCEPTION;
    }
    if (w < 0 || h < 0) {
        return JS_ThrowRangeError(ctx, "content size must be non-negative");
    }
    static_cast<UI::ScrollView*>(ud->view)->set_content_size(static_cast<uint16_t>(w), static_cast<uint16_t>(h));
    return JS_UNDEFINED;
}

inline JSValue js_scroll_view_get_scroll_x(JSContext* ctx, JSValueConst this_val, int /*argc*/,
                                           JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SCROLL_VIEW) {
        return JS_ThrowTypeError(ctx, "get_scroll_x requires a ScrollView");
    }
    return JS_NewInt32(ctx, static_cast<UI::ScrollView*>(ud->view)->get_scroll_x());
}

inline JSValue js_scroll_view_get_scroll_y(JSContext* ctx, JSValueConst this_val, int /*argc*/,
                                           JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (ud->kind != JsViewKind::SCROLL_VIEW) {
        return JS_ThrowTypeError(ctx, "get_scroll_y requires a ScrollView");
    }
    return JS_NewInt32(ctx, static_cast<UI::ScrollView*>(ud->view)->get_scroll_y());
}

// 通用几何与状态方法
inline JSValue js_view_set_position(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "set_position(x, y) requires 2 arguments");
    }
    int32_t x = 0, y = 0;
    if (JS_ToInt32(ctx, &x, argv[0]) || JS_ToInt32(ctx, &y, argv[1])) {
        return JS_EXCEPTION;
    }
    ud->view->set_position(static_cast<int16_t>(x), static_cast<int16_t>(y));
    return JS_UNDEFINED;
}

inline JSValue js_view_set_size(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "set_size(w, h) requires 2 arguments");
    }
    int32_t w = 0, h = 0;
    if (JS_ToInt32(ctx, &w, argv[0]) || JS_ToInt32(ctx, &h, argv[1])) {
        return JS_EXCEPTION;
    }
    if (w < 0 || h < 0) {
        return JS_ThrowRangeError(ctx, "dimensions must be non-negative");
    }
    ud->view->set_size(static_cast<uint16_t>(w), static_cast<uint16_t>(h));
    return JS_UNDEFINED;
}

inline JSValue js_view_get_x(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    return JS_NewInt32(ctx, ud->view->get_x());
}

inline JSValue js_view_get_y(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    return JS_NewInt32(ctx, ud->view->get_y());
}

inline JSValue js_view_get_width(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    return JS_NewInt32(ctx, ud->view->get_width());
}

inline JSValue js_view_get_height(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    return JS_NewInt32(ctx, ud->view->get_height());
}

inline JSValue js_view_set_visible(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_visible expects a boolean argument");
    }
    int b = JS_ToBool(ctx, argv[0]);
    if (b < 0) {
        return JS_EXCEPTION;
    }
    ud->view->set_visibility(b ? UI::Visibility::VISIBLE : UI::Visibility::GONE);
    return JS_UNDEFINED;
}

inline JSValue js_view_is_visible(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, ud->view->is_visible() ? 1 : 0);
}

inline JSValue js_view_set_visibility(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_visibility expects an integer visibility constant");
    }
    int32_t vis = 0;
    if (JS_ToInt32(ctx, &vis, argv[0])) {
        return JS_EXCEPTION;
    }
    UI::Visibility v = UI::Visibility::VISIBLE;
    if (vis == 1) {
        v = UI::Visibility::INVISIBLE;
    } else if (vis == 2) {
        v = UI::Visibility::GONE;
    }
    ud->view->set_visibility(v);
    return JS_UNDEFINED;
}

inline JSValue js_view_set_enabled(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_enabled expects a boolean argument");
    }
    int b = JS_ToBool(ctx, argv[0]);
    if (b < 0) {
        return JS_EXCEPTION;
    }
    ud->view->set_enabled(b != 0);
    return JS_UNDEFINED;
}

inline JSValue js_view_is_enabled(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    return JS_NewBool(ctx, ud->view->is_enabled() ? 1 : 0);
}

inline JSValue js_view_invalidate(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    ud->view->invalidate();
    return JS_UNDEFINED;
}

inline JSValue js_view_set_on_click_listener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Expected function");
    }
    JsCallbackCtx* cb_ctx = create_callback_ctx(ctx, ud->view, argv[0]);
    ud->view->set_on_click_listener(js_view_on_click, cb_ctx, &js_ctx_release);
    return JS_UNDEFINED;
}

inline JSValue js_view_set_on_long_click_listener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Expected function");
    }
    JsCallbackCtx* cb_ctx = create_callback_ctx(ctx, ud->view, argv[0]);
    ud->view->set_on_long_click_listener(js_view_on_click, cb_ctx, &js_ctx_release);
    return JS_UNDEFINED;
}

inline JSValue js_view_set_on_double_click_listener(JSContext* ctx, JSValueConst this_val, int argc,
                                                    JSValueConst* argv) {
    JsViewUserData* ud = check_view_ud(ctx, this_val);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "Expected function");
    }
    JsCallbackCtx* cb_ctx = create_callback_ctx(ctx, ud->view, argv[0]);
    ud->view->set_on_double_click_listener(js_view_on_click, cb_ctx, &js_ctx_release);
    return JS_UNDEFINED;
}

// =============================================================================
// 全局导航与根容器绑定（aurora.ui.set_root_view / navigator_push / navigator_pop）
// =============================================================================
inline JSValue js_set_root_view(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "set_root_view expects a ViewGroup argument");
    }
    JsViewUserData* ud = check_view_ud(ctx, argv[0]);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (!is_view_group_kind(ud->kind)) {
        return JS_ThrowTypeError(ctx, "set_root_view requires a ViewGroup container");
    }
    if (ud->attached) {
        return JS_ThrowTypeError(ctx, "ViewGroup is already attached to a host hierarchy");
    }
    ud->attached = true;
    UI::UiManager::instance().set_root_view(static_cast<UI::ViewGroup*>(ud->view));
    return JS_UNDEFINED;
}

inline JSValue js_navigator_push(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "navigator_push expects a ViewGroup argument");
    }
    JsViewUserData* ud = check_view_ud(ctx, argv[0]);
    if (!ud) {
        return JS_EXCEPTION;
    }
    if (!is_view_group_kind(ud->kind)) {
        return JS_ThrowTypeError(ctx, "navigator_push requires a ViewGroup container");
    }
    if (ud->attached) {
        return JS_ThrowTypeError(ctx, "ViewGroup is already attached to a host hierarchy");
    }
    // 先预检再分配：push 被拒（栈满或转场进行中）时若已构造 JsScreen，
    // 要么泄漏 JsScreen，要么析构 JsScreen 连带释放脚本仍持有的视图。
    if (!UI::ScreenNavigator::instance().can_push()) {
        return JS_ThrowInternalError(ctx, "navigator push rejected (stack full or transition in progress)");
    }

    auto* vg = static_cast<UI::ViewGroup*>(ud->view);
    auto* screen = new JsScreen(vg);
    ud->attached = true;

    if (argc >= 2) {
        int32_t trans = 0;
        if (JS_ToInt32(ctx, &trans, argv[1]) == 0 && trans >= 0 && trans <= 9) {
            UI::ScreenNavigator::instance().push(
                screen, static_cast<UI::ScreenNavigator::TransitionType>(static_cast<uint8_t>(trans)));
            return JS_UNDEFINED;
        }
    }
    UI::ScreenNavigator::instance().push(screen);
    return JS_UNDEFINED;
}

inline JSValue js_navigator_pop(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc >= 1) {
        int32_t trans = 0;
        if (JS_ToInt32(ctx, &trans, argv[0]) == 0 && trans >= 0 && trans <= 9) {
            UI::ScreenNavigator::instance().pop(
                static_cast<UI::ScreenNavigator::TransitionType>(static_cast<uint8_t>(trans)));
            return JS_UNDEFINED;
        }
    }
    UI::ScreenNavigator::instance().pop();
    return JS_UNDEFINED;
}

inline JSValue js_navigator_pop_to_root(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc >= 1) {
        int32_t trans = 0;
        if (JS_ToInt32(ctx, &trans, argv[0]) == 0 && trans >= 0 && trans <= 9) {
            UI::ScreenNavigator::instance().pop_to_root(
                static_cast<UI::ScreenNavigator::TransitionType>(static_cast<uint8_t>(trans)));
            return JS_UNDEFINED;
        }
    }
    UI::ScreenNavigator::instance().pop_to_root();
    return JS_UNDEFINED;
}

// C++17 兼容的 JSCFunctionListEntry 构造器（规避 C99 嵌套指定初始化器警告，
// 同时利用 QuickJS 的 JS_AUTOINIT_ID_PROP 按需惰性实例化 JSFunction，消除上下文冷启动堆开销）
constexpr JSCFunctionListEntry make_cfunc_entry(const char* name, uint8_t length, JSCFunction* fn) noexcept {
    return JSCFunctionListEntry{
        name, static_cast<uint8_t>(JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE), JS_DEF_CFUNC,
        0,    {{length, static_cast<uint8_t>(JS_CFUNC_generic), {fn}}},
    };
}

inline const JSCFunctionListEntry kViewProtoFuncs[] = {
    // 容器与层级
    make_cfunc_entry("add_child", 1, js_view_add_child),
    make_cfunc_entry("addChild", 1, js_view_add_child),
    make_cfunc_entry("get_child_count", 0, js_view_get_child_count),
    make_cfunc_entry("getChildCount", 0, js_view_get_child_count),
    // TextView
    make_cfunc_entry("set_text", 1, js_text_view_set_text),
    make_cfunc_entry("setText", 1, js_text_view_set_text),
    make_cfunc_entry("get_text", 0, js_text_view_get_text),
    make_cfunc_entry("getText", 0, js_text_view_get_text),
    make_cfunc_entry("set_text_color", 2, js_text_view_set_text_color),
    make_cfunc_entry("setTextColor", 2, js_text_view_set_text_color),
    make_cfunc_entry("set_text_align", 1, js_text_view_set_text_align),
    make_cfunc_entry("setTextAlign", 1, js_text_view_set_text_align),
    make_cfunc_entry("set_scale", 1, js_text_view_set_scale),
    make_cfunc_entry("setScale", 1, js_text_view_set_scale),
    // ArcProgress & ProgressBar
    make_cfunc_entry("set_percentage", 1, js_arc_progress_set_percentage),
    make_cfunc_entry("setPercentage", 1, js_arc_progress_set_percentage),
    make_cfunc_entry("set_progress", 1, js_progress_bar_set_progress),
    make_cfunc_entry("setProgress", 1, js_progress_bar_set_progress),
    make_cfunc_entry("get_progress", 0, js_progress_bar_get_progress),
    make_cfunc_entry("getProgress", 0, js_progress_bar_get_progress),
    make_cfunc_entry("get_percentage", 0, js_progress_bar_get_percentage),
    make_cfunc_entry("getPercentage", 0, js_progress_bar_get_percentage),
    make_cfunc_entry("set_range", 2, js_view_set_range),
    make_cfunc_entry("setRange", 2, js_view_set_range),
    make_cfunc_entry("set_corner_radius", 1, js_view_set_corner_radius),
    make_cfunc_entry("setCornerRadius", 1, js_view_set_corner_radius),
    // SwitchView
    make_cfunc_entry("set_checked", 1, js_switch_set_checked),
    make_cfunc_entry("setChecked", 1, js_switch_set_checked),
    make_cfunc_entry("is_checked", 0, js_switch_is_checked),
    make_cfunc_entry("isChecked", 0, js_switch_is_checked),
    make_cfunc_entry("toggle", 0, js_switch_toggle),
    make_cfunc_entry("set_on_checked_changed_listener", 1, js_switch_set_on_checked_changed),
    make_cfunc_entry("setOnCheckedChangedListener", 1, js_switch_set_on_checked_changed),
    // SliderView
    make_cfunc_entry("set_value", 1, js_slider_set_value),
    make_cfunc_entry("setValue", 1, js_slider_set_value),
    make_cfunc_entry("get_value", 0, js_slider_get_value),
    make_cfunc_entry("getValue", 0, js_slider_get_value),
    make_cfunc_entry("set_on_value_changed_listener", 1, js_slider_set_on_value_changed),
    make_cfunc_entry("setOnValueChangedListener", 1, js_slider_set_on_value_changed),
    // ScrollView
    make_cfunc_entry("scroll_to", 2, js_scroll_view_scroll_to),
    make_cfunc_entry("scrollTo", 2, js_scroll_view_scroll_to),
    make_cfunc_entry("scroll_by", 2, js_scroll_view_scroll_by),
    make_cfunc_entry("scrollBy", 2, js_scroll_view_scroll_by),
    make_cfunc_entry("set_content_size", 2, js_scroll_view_set_content_size),
    make_cfunc_entry("setContentSize", 2, js_scroll_view_set_content_size),
    make_cfunc_entry("get_scroll_x", 0, js_scroll_view_get_scroll_x),
    make_cfunc_entry("getScrollX", 0, js_scroll_view_get_scroll_x),
    make_cfunc_entry("get_scroll_y", 0, js_scroll_view_get_scroll_y),
    make_cfunc_entry("getScrollY", 0, js_scroll_view_get_scroll_y),
    // 通用 View 属性与回调
    make_cfunc_entry("set_position", 2, js_view_set_position),
    make_cfunc_entry("setPosition", 2, js_view_set_position),
    make_cfunc_entry("set_size", 2, js_view_set_size),
    make_cfunc_entry("setSize", 2, js_view_set_size),
    make_cfunc_entry("get_x", 0, js_view_get_x),
    make_cfunc_entry("getX", 0, js_view_get_x),
    make_cfunc_entry("get_y", 0, js_view_get_y),
    make_cfunc_entry("getY", 0, js_view_get_y),
    make_cfunc_entry("get_width", 0, js_view_get_width),
    make_cfunc_entry("getWidth", 0, js_view_get_width),
    make_cfunc_entry("get_height", 0, js_view_get_height),
    make_cfunc_entry("getHeight", 0, js_view_get_height),
    make_cfunc_entry("set_visible", 1, js_view_set_visible),
    make_cfunc_entry("setVisible", 1, js_view_set_visible),
    make_cfunc_entry("is_visible", 0, js_view_is_visible),
    make_cfunc_entry("isVisible", 0, js_view_is_visible),
    make_cfunc_entry("set_visibility", 1, js_view_set_visibility),
    make_cfunc_entry("setVisibility", 1, js_view_set_visibility),
    make_cfunc_entry("set_enabled", 1, js_view_set_enabled),
    make_cfunc_entry("setEnabled", 1, js_view_set_enabled),
    make_cfunc_entry("is_enabled", 0, js_view_is_enabled),
    make_cfunc_entry("isEnabled", 0, js_view_is_enabled),
    make_cfunc_entry("invalidate", 0, js_view_invalidate),
    make_cfunc_entry("set_on_click_listener", 1, js_view_set_on_click_listener),
    make_cfunc_entry("setOnClickListener", 1, js_view_set_on_click_listener),
    make_cfunc_entry("set_on_long_click_listener", 1, js_view_set_on_long_click_listener),
    make_cfunc_entry("setOnLongClickListener", 1, js_view_set_on_long_click_listener),
    make_cfunc_entry("set_on_double_click_listener", 1, js_view_set_on_double_click_listener),
    make_cfunc_entry("setOnDoubleClickListener", 1, js_view_set_on_double_click_listener),
};

inline const JSCFunctionListEntry kUiNamespaceFuncs[] = {
    make_cfunc_entry("ViewGroup", 4, js_view_group_new),
    make_cfunc_entry("TextView", 4, js_text_view_new),
    make_cfunc_entry("ArcProgress", 5, js_arc_progress_new),
    make_cfunc_entry("Button", 6, js_button_new),
    make_cfunc_entry("ProgressBar", 4, js_progress_bar_new),
    make_cfunc_entry("SwitchView", 2, js_switch_view_new),
    make_cfunc_entry("SliderView", 4, js_slider_view_new),
    make_cfunc_entry("ScrollView", 4, js_scroll_view_new),
    make_cfunc_entry("set_root_view", 1, js_set_root_view),
    make_cfunc_entry("setRootView", 1, js_set_root_view),
    make_cfunc_entry("navigator_push", 1, js_navigator_push),
    make_cfunc_entry("navigatorPush", 1, js_navigator_push),
    make_cfunc_entry("navigator_pop", 0, js_navigator_pop),
    make_cfunc_entry("navigatorPop", 0, js_navigator_pop),
    make_cfunc_entry("navigator_pop_to_root", 0, js_navigator_pop_to_root),
    make_cfunc_entry("navigatorPopToRoot", 0, js_navigator_pop_to_root),
};

inline void ensure_view_class_proto(JSContext* ctx) {
    const JSClassID view_cid = js_view_class_id();
    JSValue existing_proto = JS_GetClassProto(ctx, view_cid);
    if (JS_IsObject(existing_proto)) {
        JS_FreeValue(ctx, existing_proto);
        return;
    }
    JS_FreeValue(ctx, existing_proto);

    JSRuntime* rt = JS_GetRuntime(ctx);
    if (!JS_IsRegisteredClass(rt, view_cid)) {
        JSClassDef view_class_def{};
        view_class_def.class_name = "aurora.ui.View";
        view_class_def.finalizer = &js_view_finalizer;
        view_class_def.gc_mark = &js_view_gc_mark;
        JS_NewClass(rt, view_cid, &view_class_def);
    }

    const JSClassID guard_cid = js_ctx_guard_class_id();
    if (!JS_IsRegisteredClass(rt, guard_cid)) {
        JSClassDef guard_class_def{};
        guard_class_def.class_name = "aurora.ui.CtxGuard";
        guard_class_def.finalizer = &js_ctx_guard_finalizer;
        JS_NewClass(rt, guard_cid, &guard_class_def);
    }

    // 绑定上下文生命周期守卫：JS_FreeContext 释放 class_proto 时自动回收残留回调
    JSValue existing_guard = JS_GetClassProto(ctx, guard_cid);
    if (JS_IsObject(existing_guard)) {
        JS_FreeValue(ctx, existing_guard);
    } else {
        JS_FreeValue(ctx, existing_guard);
        JSValue guard = JS_NewObjectProtoClass(ctx, JS_NULL, guard_cid);
        if (!JS_IsException(guard)) {
            JS_SetOpaque(guard, ctx);
            JS_SetClassProto(ctx, guard_cid, guard);
        }
    }

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, kViewProtoFuncs,
                               static_cast<int>(sizeof(kViewProtoFuncs) / sizeof(kViewProtoFuncs[0])));
    JS_SetClassProto(ctx, view_cid, proto);
}

} // namespace jsui
} // namespace aurora

// 提取 JSValue 对应的 JsViewUserData*（非 aurora.ui.View 对象时返回 nullptr，不抛异常）
inline JsViewUserData* js_to_view_ud(JSContext* /*ctx*/, JSValueConst val) noexcept {
    if (!JS_IsObject(val)) {
        return nullptr;
    }
    return static_cast<JsViewUserData*>(JS_GetOpaque(val, aurora::jsui::js_view_class_id()));
}

// 显式清理指定 JSContext 下残留的 UI 回调（供宿主 teardown 主动调用；
// 即使不显式调用，JS_FreeContext 时 CtxGuard finalizer 也会兜底执行）
inline void js_cleanup_aurora_ui_callbacks(JSContext* ctx) noexcept {
    if (ctx) {
        aurora::jsui::detach_callbacks_for_context(ctx);
    }
}

// 在给定 JSContext 上注册 aurora.ui 命名空间与 aurora.ui.View 原型方法
inline void js_open_aurora_ui(JSContext* ctx) {
    if (!ctx) {
        return;
    }
    using namespace aurora::jsui;

    JSRuntime* rt = JS_GetRuntime(ctx);
    const JSClassID view_cid = js_view_class_id();
    if (!JS_IsRegisteredClass(rt, view_cid)) {
        JSClassDef view_class_def{};
        view_class_def.class_name = "aurora.ui.View";
        view_class_def.finalizer = &js_view_finalizer;
        view_class_def.gc_mark = &js_view_gc_mark;
        JS_NewClass(rt, view_cid, &view_class_def);
    }

    // 挂载到 globalThis.aurora.ui（若 aurora 尚未创建则自动新建）
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue aurora_obj = JS_GetPropertyStr(ctx, global_obj, "aurora");
    if (!JS_IsObject(aurora_obj)) {
        JS_FreeValue(ctx, aurora_obj);
        aurora_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global_obj, "aurora", JS_DupValue(ctx, aurora_obj));
    }

    JSValue ui_obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ui_obj, kUiNamespaceFuncs,
                               static_cast<int>(sizeof(kUiNamespaceFuncs) / sizeof(kUiNamespaceFuncs[0])));

    // 对齐与可见性枚举常量
    JS_SetPropertyStr(ctx, ui_obj, "ALIGN_LEFT", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, ui_obj, "ALIGN_CENTER", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, ui_obj, "ALIGN_RIGHT", JS_NewInt32(ctx, 2));
    JS_SetPropertyStr(ctx, ui_obj, "VISIBLE", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, ui_obj, "INVISIBLE", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, ui_obj, "GONE", JS_NewInt32(ctx, 2));

    JS_SetPropertyStr(ctx, aurora_obj, "ui", ui_obj);
    JS_FreeValue(ctx, aurora_obj);
    JS_FreeValue(ctx, global_obj);
}

#endif // AURORA_JS_UI_BINDING_HPP
