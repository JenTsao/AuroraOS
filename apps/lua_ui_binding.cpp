#include "lua_ui_binding.hpp"
#include "../ui/ui_manager.hpp"
#include "../ui/view_group.hpp"
#include "../ui/widgets/text_view.hpp"
#include "../ui/widgets/arc_progress.hpp"
#include "../kernel/mm/memory.hpp"

extern "C" {
#include "../3rdparty/lua/lualib.h"
#include "../3rdparty/lua/lauxlib.h"
}

using namespace UI;

struct LuaCallbackCtx {
    lua_State* L;
    int ref;
};

static void lua_view_on_click(View* /*v*/, void* ctx) {
    LuaCallbackCtx* c = static_cast<LuaCallbackCtx*>(ctx);
    lua_rawgeti(c->L, LUA_REGISTRYINDEX, c->ref);
    lua_pcall(c->L, 0, 0, 0);
}

static ViewUserData* get_view_ud(lua_State* L, int index) {
    void* ud = luaL_checkudata(L, index, "aurora.ui.View");
    luaL_argcheck(L, ud != nullptr, index, "`View` expected");
    return static_cast<ViewUserData*>(ud);
}

static View* check_view(lua_State* L, int index) {
    return get_view_ud(L, index)->view;
}

static int text_view_new(lua_State* L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    const char* text = luaL_checkstring(L, 3);
    int fg = luaL_checkinteger(L, 4);
    int bg = luaL_optinteger(L, 5, 0);
    int scale = luaL_optinteger(L, 6, 2);

    TextView* tv = new TextView(x, y, text, fg, bg, scale);

    ViewUserData* ud = static_cast<ViewUserData*>(lua_newuserdata(L, sizeof(ViewUserData)));
    ud->view = tv;
    ud->attached = false; // 新建视图归 Lua 持有，挂载宿主后移交所有权

    luaL_getmetatable(L, "aurora.ui.View");
    lua_setmetatable(L, -2);
    return 1;
}

static int view_group_new(lua_State* L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);

    ViewGroup* vg = new ViewGroup(x, y, w, h);

    ViewUserData* ud = static_cast<ViewUserData*>(lua_newuserdata(L, sizeof(ViewUserData)));
    ud->view = vg;
    ud->attached = false;

    luaL_getmetatable(L, "aurora.ui.View");
    lua_setmetatable(L, -2);
    return 1;
}

static int arc_progress_new(lua_State* L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int r = luaL_checkinteger(L, 3);
    int val = luaL_checkinteger(L, 4);
    int color = luaL_checkinteger(L, 5);

    ArcProgress* arc = new ArcProgress(x, y, r, val, color);

    ViewUserData* ud = static_cast<ViewUserData*>(lua_newuserdata(L, sizeof(ViewUserData)));
    ud->view = arc;
    ud->attached = false;

    luaL_getmetatable(L, "aurora.ui.View");
    lua_setmetatable(L, -2);
    return 1;
}

static int view_add_child(lua_State* L) {
    ViewUserData* parent_ud = get_view_ud(L, 1);
    ViewUserData* child_ud = get_view_ud(L, 2);

    ViewGroup* vg = static_cast<ViewGroup*>(parent_ud->view);
    View* child = child_ud->view;
    vg->add_child(child);

    // add_child 在子控件槽满 (MAX_CHILDREN) 时会静默丢弃。仅当挂载真正被
    // 接受（parent 已回填）才移交所有权：被拒收的视图仍归 Lua，
    // 由 __gc 回收，避免宿主未接管而造成泄漏。
    if (child->get_parent() == parent_ud->view) {
        child_ud->attached = true;
    }
    return 0;
}

static int text_view_set_text(lua_State* L) {
    View* v = check_view(L, 1);
    const char* text = luaL_checkstring(L, 2);
    static_cast<TextView*>(v)->set_text(text);
    return 0;
}

static int arc_progress_set_percentage(lua_State* L) {
    View* v = check_view(L, 1);
    int percent = luaL_checkinteger(L, 2);
    static_cast<ArcProgress*>(v)->set_percentage(percent);
    return 0;
}

static int set_root_view(lua_State* L) {
    ViewUserData* ud = get_view_ud(L, 1);
    // UiManager::set_root_view 仅保存指针、不负责释放。标记 attached 表示
    // 该视图的生命周期引用由宿主持有，__gc 不得删除，避免 GC 在 Lua 态
    // 存续期间回收仍在根槽位上的视图造成悬空。
    ud->attached = true;
    UiManager::instance().set_root_view(static_cast<ViewGroup*>(ud->view));
    return 0;
}

// LuaCallbackCtx 的释放钩子：由 View 在槽被覆盖或析构时调用。
// 顺带 luaL_unref 释放 registry 中的函数引用（旧版只 delete 对象、
// 从不 unref，registry 引用会随每次 set_on_click_listener 累积泄漏）。
static void lua_ctx_release(void* p) {
    LuaCallbackCtx* c = static_cast<LuaCallbackCtx*>(p);
    if (c->L) {
        luaL_unref(c->L, LUA_REGISTRYINDEX, c->ref);
        c->L = nullptr;
    }
    ::operator delete(p);
}

static int view_set_on_click_listener(lua_State* L) {
    View* v = check_view(L, 1);
    if (!lua_isfunction(L, 2)) {
        return luaL_error(L, "Expected function");
    }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    LuaCallbackCtx* ctx = new LuaCallbackCtx{L, ref};
    // set_on_click_listener 会先 release 旧槽（释放旧 ctx 并 luaL_unref，
    // 顺带修掉覆盖泄漏），再接管新 ctx 的所有权（deleter 非空）。
    v->set_on_click_listener(lua_view_on_click, ctx, &lua_ctx_release);
    return 0;
}

// --------------------------------------------------------
// ScreenNavigator Bindings
// --------------------------------------------------------
#include "../ui/screen.hpp"
#include "../ui/screen_navigator.hpp"

// We create a LuaScreen to wrap lua lifecycle callbacks if needed, or simply allow pushing ViewGroups.
// Wait, ScreenNavigator takes a Screen. We need a Screen wrapper.
class LuaScreen : public Screen {
    ViewGroup* root_;

public:
    LuaScreen(ViewGroup* root) : root_(root) {
        // We make the LuaScreen's view group the root_
        // Screen is a ViewGroup itself, so we can just add the root_ as a child.
        add_child(root_);
    }
};

static int navigator_push(lua_State* L) {
    ViewUserData* ud = get_view_ud(L, 1);
    // 先预检再分配：push 被拒（栈满/转场中）时若先构造了 LuaScreen，
    // 要么泄漏屏幕，要么销毁屏幕连带销毁脚本仍持有 userdata 的视图（悬空）。
    if (!ScreenNavigator::instance().can_push()) {
        return luaL_error(L, "navigator push rejected (stack full or transition in progress)");
    }
    ViewGroup* vg = static_cast<ViewGroup*>(ud->view);
    LuaScreen* screen = new LuaScreen(vg);
    // LuaScreen 构造即 add_child 接管视图所有权
    ud->attached = true;
    ScreenNavigator::instance().push(screen);
    return 0;
}

static int navigator_pop(lua_State* /*L*/) {
    ScreenNavigator::instance().pop();
    return 0;
}

// --------------------------------------------------------
// userdata 终结器：回收未被宿主接管的孤儿视图
// --------------------------------------------------------
// 脚本创建了视图但未挂入宿主视图树（未被 create_ui 返回、未被
// add_child/navigator_push 接管）时，若无此终结器，视图对象将随
// Lua 态存活至 lua_close 且永不释放——表盘反复切换会持续掏空内核堆。
// 已移交宿主 (attached=true) 的视图由宿主 ViewGroup 析构链释放，
// 此处不得重复 delete。
static int view_gc(lua_State* L) {
    void* ud = luaL_testudata(L, 1, "aurora.ui.View");
    if (ud != nullptr) {
        ViewUserData* u = static_cast<ViewUserData*>(ud);
        if (!u->attached && u->view != nullptr) {
            delete u->view;
            u->view = nullptr;
        }
    }
    return 0;
}

static const struct luaL_Reg ui_funcs[] = {{"TextView", text_view_new},
                                           {"ViewGroup", view_group_new},
                                           {"ArcProgress", arc_progress_new},
                                           {"set_root_view", set_root_view},
                                           {"navigator_push", navigator_push},
                                           {"navigator_pop", navigator_pop},
                                           {NULL, NULL}};

static const struct luaL_Reg view_methods[] = {{"add_child", view_add_child},
                                               {"set_text", text_view_set_text},
                                               {"set_percentage", arc_progress_set_percentage},
                                               {"set_on_click_listener", view_set_on_click_listener},
                                               {NULL, NULL}};

void luaopen_aurora_ui(lua_State* L) {
    luaL_newmetatable(L, "aurora.ui.View");
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, view_methods, 0);
    lua_pushcfunction(L, view_gc);
    lua_setfield(L, -2, "__gc"); // 孤儿视图由 GC 回收（所有权契约见 ViewUserData）
    lua_pop(L, 1);

    lua_getglobal(L, "aurora");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setglobal(L, "aurora");
        lua_getglobal(L, "aurora");
    }

    lua_newtable(L);
    luaL_setfuncs(L, ui_funcs, 0);
    lua_setfield(L, -2, "ui");

    lua_pop(L, 1);
}

// 自定义 luaL_openlibs —— 仅注册我们实际编译的 Lua 标准库子集。
// 原版 linit.c 会引用 luaopen_package/luaopen_io/luaopen_os/luaopen_string/luaopen_math，
// 但这些库的源文件（loadlib.c/liolib.c/loslib.c/lstrlib.c/lmathlib.c）
// 在 CMakeLists.txt 中被排除以节省 RAM，因此在此提供精简版。
extern "C" {
extern int luaopen_base(lua_State* L);
extern int luaopen_coroutine(lua_State* L);
extern int luaopen_table(lua_State* L);
extern int luaopen_utf8(lua_State* L);
extern int luaopen_debug(lua_State* L);

static const luaL_Reg loadedlibs[] = {{"_G", luaopen_base},
                                      {LUA_COLIBNAME, luaopen_coroutine},
                                      {LUA_TABLIBNAME, luaopen_table},
                                      {LUA_UTF8LIBNAME, luaopen_utf8},
                                      {LUA_DBLIBNAME, luaopen_debug},
                                      {NULL, NULL}};

void luaL_openlibs(lua_State* L) {
    const luaL_Reg* lib;
    for (lib = loadedlibs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
}
}
