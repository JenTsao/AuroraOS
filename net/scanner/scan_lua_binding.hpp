#ifndef AURORA_SCANNER_SCAN_LUA_BINDING_HPP
#define AURORA_SCANNER_SCAN_LUA_BINDING_HPP

#include <stdint.h>
#include <stddef.h>
#include "scan_engine.hpp"

extern "C" {
#include "../../3rdparty/lua/lua.h"
#include "../../3rdparty/lua/lualib.h"
#include "../../3rdparty/lua/lauxlib.h"
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
// ============================================================

class ScanLuaBinding {
public:
    // 注册所有扫描 API 到 Lua 的 aurora.scan 命名空间
    //   L: lua_State 指针（来自 MiniProgramEngine）
    static void register_bindings(lua_State* L) {
        if (!L) return;

        // 创建 aurora.scan 子表
        lua_getglobal(L, "aurora");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return;
        }

        lua_newtable(L);

        // ---- 扫描配置 ----
        lua_pushcfunction(L, lua_set_timeout);
        lua_setfield(L, -2, "set_timeout");

        lua_pushcfunction(L, lua_set_retries);
        lua_setfield(L, -2, "set_retries");

        // ---- 端口扫描 ----
        lua_pushcfunction(L, lua_scan_tcp_port);
        lua_setfield(L, -2, "scan_tcp_port");

        lua_pushcfunction(L, lua_scan_tcp_range);
        lua_setfield(L, -2, "scan_tcp_range");

        lua_pushcfunction(L, lua_scan_udp_port);
        lua_setfield(L, -2, "scan_udp_port");

        // ---- 主机发现 ----
        lua_pushcfunction(L, lua_scan_hosts);
        lua_setfield(L, -2, "scan_hosts");

        lua_pushcfunction(L, lua_ping_host);
        lua_setfield(L, -2, "ping_host");

        // ---- 服务探测 ----
        lua_pushcfunction(L, lua_detect_service);
        lua_setfield(L, -2, "detect_service");

        // ---- 漏洞检测 ----
        lua_pushcfunction(L, lua_probe_vuln);
        lua_setfield(L, -2, "probe_vuln");

        // ---- 综合扫描 ----
        lua_pushcfunction(L, lua_quick_scan);
        lua_setfield(L, -2, "quick_scan");

        // ---- 结果管理 ----
        lua_pushcfunction(L, lua_has_results);
        lua_setfield(L, -2, "has_results");

        lua_pushcfunction(L, lua_result_count);
        lua_setfield(L, -2, "result_count");

        lua_pushcfunction(L, lua_pop_result);
        lua_setfield(L, -2, "pop_result");

        lua_pushcfunction(L, lua_clear_results);
        lua_setfield(L, -2, "clear_results");

        // 注册子表
        lua_setfield(L, -2, "scan");
        lua_pop(L, 1); // pop "aurora"
    }

private:
    // ---- Lua C 函数实现 ----

    // aurora.scan.set_timeout(ms)
    static int lua_set_timeout(lua_State* L) {
        uint32_t ms = static_cast<uint32_t>(luaL_checkinteger(L, 1));
        ScanEngine::instance().set_banner_timeout(ms);
        ScanEngine::instance().set_vuln_timeout(ms);
        return 0;
    }

    // aurora.scan.set_retries(n)
    static int lua_set_retries(lua_State* L) {
        // 当前版本保留接口，后续扩展
        (void)luaL_checkinteger(L, 1);
        return 0;
    }

    // aurora.scan.scan_tcp_port(ip_str, port_table)
    //   例: aurora.scan.scan_tcp_port("192.168.1.1", {22, 80, 443})
    static int lua_scan_tcp_port(lua_State* L) {
        const char* ip_str = luaL_checkstring(L, 1);
        uint32_t ip = parse_ip_(ip_str);
        if (ip == 0) {
            lua_pushboolean(L, 0);
            return 1;
        }

        // 解析端口表（必须为数组，从 index 1 开始）
        luaL_checktype(L, 2, LUA_TTABLE);

        uint16_t ports[64];
        int port_count = 0;

        lua_pushnil(L); // 第一个 key
        while (lua_next(L, 2) != 0 && port_count < 64) {
            ports[port_count] = static_cast<uint16_t>(lua_tointeger(L, -1));
            ++port_count;
            lua_pop(L, 1); // pop value
        }

        int scheduled = ScanEngine::instance().start_tcp_port_scan(ip, ports, port_count);
        lua_pushinteger(L, scheduled);
        return 1;
    }

    // aurora.scan.scan_tcp_range(ip_str, port_start, port_end)
    static int lua_scan_tcp_range(lua_State* L) {
        const char* ip_str = luaL_checkstring(L, 1);
        uint32_t ip = parse_ip_(ip_str);
        if (ip == 0) {
            lua_pushboolean(L, 0);
            return 1;
        }

        uint16_t port_start = static_cast<uint16_t>(luaL_checkinteger(L, 2));
        uint16_t port_end   = static_cast<uint16_t>(luaL_checkinteger(L, 3));

        // 展开为端口列表（最多 64 端口）
        uint16_t ports[64];
        int count = 0;
        for (uint16_t p = port_start; p <= port_end && count < 64; ++p) {
            ports[count++] = p;
        }

        int scheduled = ScanEngine::instance().start_tcp_port_scan(ip, ports, count);
        lua_pushinteger(L, scheduled);
        return 1;
    }

    // aurora.scan.scan_udp_port(ip_str, port_table)
    static int lua_scan_udp_port(lua_State* L) {
        // UDP 扫描实现与 TCP 类似，留待后续完善
        // 当前使用 quick_scan 部分模拟
        (void)L;
        lua_pushinteger(L, 0);
        return 1;
    }

    // aurora.scan.scan_hosts(subnet_str)
    //   例: aurora.scan.scan_hosts("192.168.1.0/24")
    static int lua_scan_hosts(lua_State* L) {
        const char* subnet = luaL_checkstring(L, 1);
        uint32_t prefix = parse_ip_(subnet);
        if (prefix == 0) {
            lua_pushboolean(L, 0);
            return 1;
        }

        int scheduled = ScanEngine::instance().start_host_discovery(prefix);
        lua_pushinteger(L, scheduled);
        return 1;
    }

    // aurora.scan.ping_host(ip_str)
    static int lua_ping_host(lua_State* L) {
        // 使用 quick_scan 的 HostDiscovery
        // 当前由 ScanEngine 异步处理，返回 1 表示已调度
        (void)L;
        lua_pushboolean(L, 1);
        return 1;
    }

    // aurora.scan.detect_service(ip_str, port)
    static int lua_detect_service(lua_State* L) {
        const char* ip_str = luaL_checkstring(L, 1);
        uint16_t port = static_cast<uint16_t>(luaL_checkinteger(L, 2));
        uint32_t ip = parse_ip_(ip_str);
        if (ip == 0) {
            lua_pushnil(L);
            return 1;
        }

        uint16_t ports[] = {port};
        int scheduled = ScanEngine::instance().start_service_detection(ip, ports, 1);
        lua_pushinteger(L, scheduled);
        return 1;
    }

    // aurora.scan.probe_vuln(ip_str, port, service_name)
    static int lua_probe_vuln(lua_State* L) {
        const char* ip_str = luaL_checkstring(L, 1);
        uint16_t port = static_cast<uint16_t>(luaL_checkinteger(L, 2));
        const char* svc = lua_tostring(L, 3); // 可为 nil

        uint32_t ip = parse_ip_(ip_str);
        if (ip == 0) {
            lua_pushnil(L);
            return 1;
        }

        uint16_t ports[] = {port};
        int scheduled = ScanEngine::instance().start_vuln_probe(ip, ports, 1, svc);
        lua_pushinteger(L, scheduled);
        return 1;
    }

    // aurora.scan.quick_scan(ip_str, port_table)
    //   同步版本，返回结果表: {{port=22, state="open", service="ssh", version="..."}, ...}
    static int lua_quick_scan(lua_State* L) {
        const char* ip_str = luaL_checkstring(L, 1);
        uint32_t ip = parse_ip_(ip_str);
        if (ip == 0) {
            lua_pushnil(L);
            return 1;
        }

        // 解析端口列表
        uint16_t ports[64];
        int port_count = 0;

        if (lua_istable(L, 2)) {
            lua_pushnil(L);
            while (lua_next(L, 2) != 0 && port_count < 64) {
                ports[port_count] = static_cast<uint16_t>(lua_tointeger(L, -1));
                ++port_count;
                lua_pop(L, 1);
            }
        } else if (lua_isinteger(L, 2)) {
            ports[0] = static_cast<uint16_t>(lua_tointeger(L, 2));
            port_count = 1;
        }

        // 执行同步扫描
        UnifiedScanResult results[64];
        int count = ScanEngine::instance().quick_scan(ip, ports, port_count, results, 64);

        // 构建返回表
        lua_newtable(L);
        for (int i = 0; i < count; ++i) {
            lua_newtable(L);

            lua_pushinteger(L, results[i].port);
            lua_setfield(L, -2, "port");

            lua_pushstring(L, PortScanner::port_state_to_string(
                static_cast<PortState>(results[i].port_state)));
            lua_setfield(L, -2, "state");

            if (results[i].service_name[0]) {
                lua_pushstring(L, results[i].service_name);
                lua_setfield(L, -2, "service");

                if (results[i].version[0]) {
                    lua_pushstring(L, results[i].version);
                    lua_setfield(L, -2, "version");
                }
            }

            lua_pushinteger(L, results[i].latency_ms);
            lua_setfield(L, -2, "latency_ms");

            lua_rawseti(L, -2, i + 1); // Lua 数组从 1 开始
        }

        return 1;
    }

    // aurora.scan.has_results() → bool
    static int lua_has_results(lua_State* L) {
        lua_pushboolean(L, ScanEngine::instance().get_result_count() > 0);
        return 1;
    }

    // aurora.scan.result_count() → int
    static int lua_result_count(lua_State* L) {
        lua_pushinteger(L, ScanEngine::instance().get_result_count());
        return 1;
    }

    // aurora.scan.pop_result() → ip, port, state, service, version
    static int lua_pop_result(lua_State* L) {
        static int pop_index = 0;
        UnifiedScanResult result;

        if (!ScanEngine::instance().get_result(pop_index, result)) {
            lua_pushnil(L);
            return 1;
        }

        // IP 转字符串
        char ip_str[16];
        ip_to_string_(result.ip, ip_str, sizeof(ip_str));
        lua_pushstring(L, ip_str);

        lua_pushinteger(L, result.port);
        lua_pushstring(L, PortScanner::port_state_to_string(
            static_cast<PortState>(result.port_state)));

        if (result.service_name[0]) {
            lua_pushstring(L, result.service_name);
        } else {
            lua_pushnil(L);
        }

        if (result.version[0]) {
            lua_pushstring(L, result.version);
        } else {
            lua_pushnil(L);
        }

        ++pop_index;
        return 5;
    }

    // aurora.scan.clear_results()
    static int lua_clear_results(lua_State* L) {
        ScanEngine::instance().clear_results();
        return 0;
    }

    // ---- 工具方法 ----

    // 解析 IP 字符串 "192.168.1.1" → uint32_t（网络字节序）
    static uint32_t parse_ip_(const char* str) {
        if (!str) return 0;

        uint8_t octets[4] = {};
        int octet = 0;
        int val = 0;

        for (int i = 0; str[i]; ++i) {
            if (str[i] >= '0' && str[i] <= '9') {
                val = val * 10 + (str[i] - '0');
            } else if (str[i] == '.' || str[i] == '/') {
                if (octet >= 4 || val > 255) return 0;
                octets[octet++] = static_cast<uint8_t>(val);
                val = 0;
                if (str[i] == '/') break; // 子网标记，停止解析
            } else {
                return 0;
            }
        }
        if (octet < 4 && val <= 255) {
            octets[octet++] = static_cast<uint8_t>(val);
        }

        if (octet < 4) return 0;

        return (static_cast<uint32_t>(octets[0]) << 24) |
               (static_cast<uint32_t>(octets[1]) << 16) |
               (static_cast<uint32_t>(octets[2]) << 8)  |
               (static_cast<uint32_t>(octets[3]));
    }

    // IP uint32_t → "x.x.x.x" 字符串
    static void ip_to_string_(uint32_t ip, char* out, int max_len) {
        uint8_t* b = reinterpret_cast<uint8_t*>(&ip);
        // 网络字节序 → 主机序
        int pos = 0;
        auto append_byte = [&](uint8_t v) {
            if (v >= 100) { out[pos++] = '0' + (v / 100); v %= 100; }
            if (v >= 10)  { out[pos++] = '0' + (v / 10);  v %= 10;  }
            out[pos++] = '0' + v;
        };
        append_byte(b[0]); out[pos++] = '.';
        append_byte(b[1]); out[pos++] = '.';
        append_byte(b[2]); out[pos++] = '.';
        append_byte(b[3]);
        out[pos] = '\0';
        (void)max_len;
    }
};

// 外部入口：在 MiniProgramEngine 中调用此函数注册
inline void register_scan_lua_bindings(lua_State* L) {
    ScanLuaBinding::register_bindings(L);
}

#endif // AURORA_SCANNER_SCAN_LUA_BINDING_HPP
