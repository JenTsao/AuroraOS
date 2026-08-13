#include "scan_handlers.hpp"

extern "C" {
#include "../net_client.hpp"
}

// ---- 工具函数 (类似 scan_engine.cpp 中的 copy_str_) ----
static void copy_str_h(char* dst, const char* src, int max_len) {
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

bool TcpPortScanHandler::execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept {
    PortScanner scanner;
    PortResult pr = scanner.tcp_connect_scan(job.ip, job.port);
    result.port_state = static_cast<uint8_t>(pr.state);
    result.latency_ms = pr.latency_ms;
    return true;
}

bool UdpPortScanHandler::execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept {
    PortScanner scanner;
    PortResult pr = scanner.udp_scan(job.ip, job.port);
    result.port_state = static_cast<uint8_t>(pr.state);
    result.latency_ms = pr.latency_ms;
    return true;
}

bool AckPortScanHandler::execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept {
    PortScanner scanner;
    PortResult pr = scanner.ack_scan(job.ip, job.port);
    result.port_state = static_cast<uint8_t>(pr.state);
    result.latency_ms = pr.latency_ms;
    return true;
}

ArpDiscoveryHandler::ArpDiscoveryHandler(struct netif* netif) {
    discovery_.init(netif);
}

bool ArpDiscoveryHandler::execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept {
    HostResult hr = discovery_.arp_scan(job.ip);
    result.host_state = static_cast<uint8_t>(hr.state);
    result.latency_ms = hr.latency_ms;
    return true;
}

IcmpPingHandler::IcmpPingHandler(struct netif* netif) {
    discovery_.init(netif);
}

bool IcmpPingHandler::execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept {
    HostResult hr = discovery_.icmp_ping(job.ip);
    result.host_state = static_cast<uint8_t>(hr.state);
    result.latency_ms = hr.latency_ms;
    return true;
}

bool ServiceDetectHandler::execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept {
    ServiceDetector detector;
    ServiceInfo si{};
    if (detector.detect_service(job.ip, job.port, si)) {
        copy_str_h(result.service_name, si.service, sizeof(result.service_name));
        copy_str_h(result.version, si.version, sizeof(result.version));
        copy_str_h(result.banner, si.banner, sizeof(result.banner));
    }
    result.port_state = static_cast<uint8_t>(PortState::Open);
    return true;
}

// ScanEngine needs to expose `find_service_for_target_` publicly, or VulnProbeHandler needs to query the engine.
// I will modify `ScanEngine` to expose `find_service_for_target`.

bool VulnProbeHandler::execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept {
    char svc_name[32] = {};
    if (engine_) {
        engine_->find_service_for_target(job.ip, job.port, svc_name, sizeof(svc_name));
    }

    VulnProbe probe;
    VulnResult vulns[4];
    int vcount = probe.probe_vulnerabilities(job.ip, job.port, svc_name, vulns, 4);

    if (vcount > 0 && vulns[0].vulnerable) {
        copy_str_h(result.cve_id, vulns[0].cve_id, sizeof(result.cve_id));
        result.severity = static_cast<uint8_t>(vulns[0].severity);
    }
    return true;
}
