#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../net/firewall/firewall_engine.hpp"

using namespace auroraos;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > 1500) return 0; // limit to MTU
    
    // Simulate network packet input to the firewall engine
    net::firewall::FirewallEngine::instance().inspect_packet(data, size, net::firewall::Direction::IN);
    
    return 0;
}
