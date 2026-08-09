#ifndef AURORA_SCANNER_SCAN_LUA_BINDING_HPP
#define AURORA_SCANNER_SCAN_LUA_BINDING_HPP

#include <stdint.h>
#include <stddef.h>
#include "scan_engine.hpp"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// ============================================================
// Lua 扫描策略绑定 -- 将 ScanEngine 功能暴露给 MiniProgramEngine
//
// Lua 脚本示例:
//   aurora.scan.set_timeout(2000)
//   aurora.scan.scan_tcp_port("192.168.1.1", {22, 80, 443})
//   aurora.scan.scan_hosts("192.168.1.0/24")
//
//   function on_scan_tick()
//       while aurora.scan.has_results() do
//           local ip, port, state, svc = aurora.scan.pop_result()
//           aurora.print(ip .. ":" .. port .. " - " .. state)
//       end
//   end
//
// 实现文件: scan_lua_binding.cpp
// ============================================================

class ScanLuaBinding {
public:
    // 注册所有扫描 API 到 Lua 的 aurora.scan 命名空间
    static void register_bindings(lua_State* L);
};

// 外部入口：在 MiniProgramEngine 中调用此函数注册
// 实现委托给 scan_lua_binding.cpp
void register_scan_lua_bindings(lua_State* L);

#endif // AURORA_SCANNER_SCAN_LUA_BINDING_HPP
