#include "firewall_audit.hpp"
#include "../../syscall/syscall.hpp"

namespace auroraos {
namespace firewall {

void FirewallAudit::log_drop(const char* reason, const uint8_t* packet, int len) {
    // For now, use sys_print for audit logging. 
    // Later this can write to VFS or send an IPC message to a log daemon.
    sys_print("[Firewall Audit] DROPPED packet (len=");
    // Cannot use complex printf in standard AuroraOS without custom libc, but we can print simple things.
    sys_print(reason);
    sys_print(")\r\n");
}

void FirewallAudit::log_accept(const uint8_t* packet, int len) {
    // Audit accepted packets if verbose logging is enabled
}

} // namespace firewall
} // namespace auroraos

