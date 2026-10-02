#ifndef AURORA_LUA_UI_BINDING_HPP
#define AURORA_LUA_UI_BINDING_HPP

extern "C" {
#include "../3rdparty/lua/lua.h"
#include "../3rdparty/lua/lauxlib.h"
}

namespace UI {
class View;
}

// Lua 侧 View 句柄与所有权标记。
//
// attached == false (默认)：视图仍归 Lua 侧持有。脚本创建后丢弃的"孤儿视图"
//   由 __gc 终结器负责 delete，防止内核堆随表盘切换持续泄漏。
// attached == true：所有权已移交宿主视图树（add_child / set_root_view /
//   navigator_push 任一挂载路径），由宿主 ViewGroup 析构链统一释放；
//   __gc 终结器不得再 delete（防双重释放）。
//
// 注意：宿主视图树销毁后，脚本不得再经 userdata 访问对应视图（悬空），
// 该约束与既有绑定语义一致。
struct ViewUserData {
    UI::View* view;
    bool attached;
};

void luaopen_aurora_ui(lua_State* L);

#endif // AURORA_LUA_UI_BINDING_HPP
