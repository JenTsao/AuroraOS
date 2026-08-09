// ============================================================
// wireless_lua_binding.cpp — Lua bindings for WiFi security
//
// Exposes aurora.wireless.* namespace to Lua scripts:
//   start_monitor()        — start WiFi monitor mode
//   stop_monitor()         — stop WiFi monitor
//   set_channel(freq)      — set capture channel
//   get_ap_table()         — return AP list as Lua table
//   get_alerts()           — return recent IDS alerts
//   get_rules()            — return active IDS rules
//   add_rule(type, sev, cnt, win, act) — add IDS rule
//   remove_rule(idx)       — remove IDS rule
//   get_stats()            — overall statistics
//   get_handshakes()       — captured handshake count
//   export_hashcat(fname)  — export handshakes as hashcat format
// ============================================================

#include "wireless_ids.hpp"
#include "beacon_analyzer.hpp"
#include "deauth_detector.hpp"
#include "handshake_capture.hpp"
#include "wifi_monitor.hpp"
#include <string.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// ============================================================
// Helper: Convert MAC to hex string "XX:XX:XX:XX:XX:XX"
// ============================================================
static void mac_to_hex_(const uint8_t* mac, char* out) {
    const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; ++i) {
        out[i * 3]     = hex[mac[i] >> 4];
        out[i * 3 + 1] = hex[mac[i] & 0x0F];
        if (i < 5) out[i * 3 + 2] = ':';
    }
    out[17] = '\0';
}

// ============================================================
// aurora.wireless.start_monitor()
// ============================================================
static int lua_start_monitor(lua_State* L) {
    WifiMonitorDevice& dev = Rtl8187lMonitor::instance();
    if (dev.get_state() == WifiMonitorDevice::DriverState::Uninitialized ||
        dev.get_state() == WifiMonitorDevice::DriverState::Error) {
        // Try RTL8812AU as fallback
        dev = Rtl8812auMonitor::instance();
    }

    if (!dev.init()) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "WiFi device init failed");
        return 2;
    }

    bool ok = dev.enter_monitor_mode();
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) lua_pushstring(L, "monitor mode failed");
    else     lua_pushnil(L);
    return 2;
}

// ============================================================
// aurora.wireless.stop_monitor()
// ============================================================
static int lua_stop_monitor(lua_State* L) {
    (void)L;
    Rtl8187lMonitor::instance().leave_monitor_mode();
    Rtl8812auMonitor::instance().leave_monitor_mode();
    return 0;
}

// ============================================================
// aurora.wireless.set_channel(freq_mhz)
// ============================================================
static int lua_set_channel(lua_State* L) {
    int freq = static_cast<int>(luaL_checkinteger(L, 1));
    if (freq < 2400 || freq > 5900) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid frequency");
        return 2;
    }

    WifiMonitorDevice& dev = Rtl8187lMonitor::instance();
    bool ok = dev.set_channel(static_cast<uint16_t>(freq));
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) lua_pushstring(L, "set channel failed");
    else     lua_pushnil(L);
    return 2;
}

// ============================================================
// aurora.wireless.get_ap_table()
// Returns a Lua table of AP information.
// ============================================================
static int lua_get_ap_table(lua_State* L) {
    BeaconAnalyzer& ba = BeaconAnalyzer::instance();
    int count = ba.get_ap_count();

    lua_newtable(L);

    for (int i = 0; i < count; ++i) {
        const ApInfo* ap = ba.get_ap(i);
        if (!ap) continue;

        lua_newtable(L);

        // SSID
        lua_pushstring(L, ap->ssid);
        lua_setfield(L, -2, "ssid");

        // BSSID (hex string)
        char bssid_str[20];
        mac_to_hex_(ap->bssid, bssid_str);
        lua_pushstring(L, bssid_str);
        lua_setfield(L, -2, "bssid");

        // Channel
        lua_pushinteger(L, ap->channel);
        lua_setfield(L, -2, "channel");

        // RSSI
        lua_pushinteger(L, ap->rssi_dbm);
        lua_setfield(L, -2, "rssi");

        // Encryption
        lua_pushstring(L, BeaconAnalyzer::encryption_name(ap->encryption));
        lua_setfield(L, -2, "encryption");

        // Flags
        lua_pushboolean(L, ap->is_hidden ? 1 : 0);
        lua_setfield(L, -2, "hidden");

        lua_pushboolean(L, ap->is_wps_supported ? 1 : 0);
        lua_setfield(L, -2, "wps");

        lua_pushboolean(L, ap->is_pmf_required ? 1 : 0);
        lua_setfield(L, -2, "pmf");

        // Beacon count
        lua_pushinteger(L, ap->beacon_count);
        lua_setfield(L, -2, "beacons");

        // Push to result table
        lua_pushinteger(L, i + 1);
        lua_insert(L, -2);
        lua_settable(L, -3);
    }

    return 1;
}

// ============================================================
// aurora.wireless.get_alerts()
// ============================================================
static int lua_get_alerts(lua_State* L) {
    WirelessIds& ids = WirelessIds::instance();
    int count = ids.get_alert_count();

    lua_newtable(L);

    int start = (count > 50) ? count - 50 : 0;
    int idx = 1;
    for (int i = start; i < count; ++i) {
        const SecurityAlert* a = ids.get_alert(i % 64);
        if (!a) continue;

        lua_newtable(L);

        lua_pushinteger(L, a->timestamp);
        lua_setfield(L, -2, "timestamp");

        lua_pushstring(L, WirelessIds::event_type_name(a->alert_type));
        lua_setfield(L, -2, "type");

        lua_pushinteger(L, a->severity);
        lua_setfield(L, -2, "severity");

        char bssid_str[20];
        mac_to_hex_(a->bssid, bssid_str);
        lua_pushstring(L, bssid_str);
        lua_setfield(L, -2, "bssid");

        lua_pushstring(L, a->description);
        lua_setfield(L, -2, "desc");

        lua_pushinteger(L, a->event_count);
        lua_setfield(L, -2, "count");

        lua_pushinteger(L, idx);
        lua_insert(L, -2);
        lua_settable(L, -3);
        ++idx;
    }

    return 1;
}

// ============================================================
// aurora.wireless.get_stats()
// ============================================================
static int lua_get_stats(lua_State* L) {
    WirelessIds& ids = WirelessIds::instance();

    lua_newtable(L);

    lua_pushinteger(L, static_cast<lua_Integer>(ids.get_total_events()));
    lua_setfield(L, -2, "total_events");

    lua_pushinteger(L, static_cast<lua_Integer>(ids.get_total_alerts()));
    lua_setfield(L, -2, "total_alerts");

    lua_pushinteger(L, ids.get_rule_count());
    lua_setfield(L, -2, "active_rules");

    // Deauth stats
    DeauthDetector& deauth = DeauthDetector::instance();
    lua_pushinteger(L, static_cast<lua_Integer>(deauth.get_total_deauths()));
    lua_setfield(L, -2, "deauths");

    lua_pushinteger(L, static_cast<lua_Integer>(deauth.get_total_disassocs()));
    lua_setfield(L, -2, "disassocs");

    lua_pushinteger(L, static_cast<lua_Integer>(deauth.get_total_spoofs()));
    lua_setfield(L, -2, "spoofs");

    // Beacon stats
    BeaconAnalyzer& ba = BeaconAnalyzer::instance();
    lua_pushinteger(L, ba.get_ap_count());
    lua_setfield(L, -2, "aps");

    lua_pushinteger(L, ba.count_hidden_ssid());
    lua_setfield(L, -2, "hidden_aps");

    lua_pushinteger(L, ba.count_weak_encryption());
    lua_setfield(L, -2, "weak_encryption");

    // Handshake stats
    HandshakeCapture& hc = HandshakeCapture::instance();
    lua_pushinteger(L, hc.get_complete_handshakes());
    lua_setfield(L, -2, "handshakes");

    lua_pushinteger(L, hc.get_pmkid_captures());
    lua_setfield(L, -2, "pmkid");

    return 1;
}

// ============================================================
// aurora.wireless.get_handshakes()
// ============================================================
static int lua_get_handshakes(lua_State* L) {
    HandshakeCapture& hc = HandshakeCapture::instance();

    lua_newtable(L);

    for (int i = 0; i < hc.get_session_count(); ++i) {
        const HandshakeSession* s = hc.get_session(i);
        if (!s) continue;

        lua_newtable(L);

        char ap_mac[20], sta_mac[20];
        mac_to_hex_(s->ap_mac, ap_mac);
        mac_to_hex_(s->sta_mac, sta_mac);

        lua_pushstring(L, ap_mac);
        lua_setfield(L, -2, "ap");

        lua_pushstring(L, sta_mac);
        lua_setfield(L, -2, "sta");

        lua_pushstring(L, s->ssid);
        lua_setfield(L, -2, "ssid");

        lua_pushboolean(L, s->complete ? 1 : 0);
        lua_setfield(L, -2, "complete");

        lua_pushboolean(L, s->pmkid_capture ? 1 : 0);
        lua_setfield(L, -2, "pmkid");

        lua_pushinteger(L, i + 1);
        lua_insert(L, -2);
        lua_settable(L, -3);
    }

    return 1;
}

// ============================================================
// aurora.wireless.get_rules()
// ============================================================
static int lua_get_rules(lua_State* L) {
    WirelessIds& ids = WirelessIds::instance();

    lua_newtable(L);

    int rules = ids.get_rule_count();
    int r = 0;
    for (int i = 0; i < 32 && r < rules; ++i) {
        // We can't directly read rule structs (private) — use public API
        // Placeholder: return rule count only
        // Full implementation would expose get_rule(index)
    }

    lua_pushinteger(L, rules);
    return 1;
}

// ============================================================
// aurora.wireless.add_rule(type_str, severity, threshold, window_ms, action)
// ============================================================
static int lua_add_rule(lua_State* L) {
    const char* type_str = luaL_checkstring(L, 1);
    int severity  = static_cast<int>(luaL_checkinteger(L, 2));
    int threshold = static_cast<int>(luaL_checkinteger(L, 3));
    int window_ms = static_cast<int>(luaL_checkinteger(L, 4));
    int action    = static_cast<int>(luaL_checkinteger(L, 5));

    // Map type string to WirelessEventType
    WirelessEventType etype = WirelessEventType::UnknownFrameType;
    for (int i = 0; i <= 9; ++i) {
        WirelessEventType t = static_cast<WirelessEventType>(i);
        const char* name = WirelessIds::event_type_name(t);
        if (name && strcmp(type_str, name) == 0) {
            etype = t;
            break;
        }
    }

    IdsRule rule{};
    rule.event_type      = etype;
    rule.min_severity    = static_cast<uint8_t>(severity);
    rule.threshold_count = static_cast<uint16_t>(threshold);
    rule.window_ms       = static_cast<uint32_t>(window_ms);
    rule.action          = static_cast<uint8_t>(action);
    rule.enabled         = true;

    bool ok = WirelessIds::instance().add_rule(rule);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ============================================================
// aurora.wireless.remove_rule(index)
// ============================================================
static int lua_remove_rule(lua_State* L) {
    int idx = static_cast<int>(luaL_checkinteger(L, 1));
    WirelessIds::instance().remove_rule(idx);
    return 0;
}

// ============================================================
// aurora.wireless.clear_rules()
// ============================================================
static int lua_clear_rules(lua_State* L) {
    (void)L;
    WirelessIds::instance().clear_rules();
    return 0;
}

// ============================================================
// Register all bindings under aurora.wireless
// ============================================================

void register_wireless_lua_bindings(lua_State* L) {
    if (!L) return;

    // Get aurora global table
    lua_getglobal(L, "aurora");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    // Create aurora.wireless sub-table
    lua_newtable(L);

    // ---- Monitor control ----
    lua_pushcfunction(L, lua_start_monitor);
    lua_setfield(L, -2, "start_monitor");

    lua_pushcfunction(L, lua_stop_monitor);
    lua_setfield(L, -2, "stop_monitor");

    lua_pushcfunction(L, lua_set_channel);
    lua_setfield(L, -2, "set_channel");

    // ---- AP data ----
    lua_pushcfunction(L, lua_get_ap_table);
    lua_setfield(L, -2, "get_ap_table");

    // ---- IDS alerts ----
    lua_pushcfunction(L, lua_get_alerts);
    lua_setfield(L, -2, "get_alerts");

    // ---- Statistics ----
    lua_pushcfunction(L, lua_get_stats);
    lua_setfield(L, -2, "get_stats");

    // ---- Handshakes ----
    lua_pushcfunction(L, lua_get_handshakes);
    lua_setfield(L, -2, "get_handshakes");

    // ---- Rules ----
    lua_pushcfunction(L, lua_get_rules);
    lua_setfield(L, -2, "get_rules");

    lua_pushcfunction(L, lua_add_rule);
    lua_setfield(L, -2, "add_rule");

    lua_pushcfunction(L, lua_remove_rule);
    lua_setfield(L, -2, "remove_rule");

    lua_pushcfunction(L, lua_clear_rules);
    lua_setfield(L, -2, "clear_rules");

    // Set aurora.wireless
    lua_setfield(L, -2, "wireless");
    lua_pop(L, 1);  // pop aurora table
}
