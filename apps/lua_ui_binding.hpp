#ifndef AURORA_LUA_UI_BINDING_HPP
#define AURORA_LUA_UI_BINDING_HPP

extern "C" {
    #include "../3rdparty/lua/lua.h"
    #include "../3rdparty/lua/lauxlib.h"
}

void luaopen_aurora_ui(lua_State* L);

#endif // AURORA_LUA_UI_BINDING_HPP
