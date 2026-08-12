#ifndef AURORAOS_FIREWALL_AUDIT_HPP
#define AURORAOS_FIREWALL_AUDIT_HPP

#include <stdint.h>

namespace auroraos {
namespace firewall {

class FirewallAudit {
public:
    static FirewallAudit& instance() {
        static FirewallAudit audit;
        return audit;
    }

    void log_drop(const char* reason, const uint8_t* packet, int len);
    void log_accept(const uint8_t* packet, int len);
    
private:
    FirewallAudit() = default;
};

} // namespace firewall
} // namespace auroraos

#endif // AURORAOS_FIREWALL_AUDIT_HPP
