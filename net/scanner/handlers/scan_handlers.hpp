#ifndef AURORA_SCANNER_HANDLERS_HPP
#define AURORA_SCANNER_HANDLERS_HPP

#include "../scan_handler.hpp"
#include "../scan_engine.hpp" // For ScanJobDesc and UnifiedScanResult
#include "../port_scanner.hpp"
#include "../host_discovery.hpp"
#include "../service_detector.hpp"
#include "../vuln_probe.hpp"

// We can define the handlers here

class TcpPortScanHandler : public IScanHandler {
public:
    bool execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept override;
};

class UdpPortScanHandler : public IScanHandler {
public:
    bool execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept override;
};

class AckPortScanHandler : public IScanHandler {
public:
    bool execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept override;
};

class ArpDiscoveryHandler : public IScanHandler {
private:
    HostDiscovery discovery_;
public:
    ArpDiscoveryHandler(struct netif* netif);
    bool execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept override;
};

class IcmpPingHandler : public IScanHandler {
private:
    HostDiscovery discovery_;
public:
    IcmpPingHandler(struct netif* netif);
    bool execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept override;
};

class ServiceDetectHandler : public IScanHandler {
public:
    bool execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept override;
};

class VulnProbeHandler : public IScanHandler {
private:
    class ScanEngine* engine_;
public:
    VulnProbeHandler(class ScanEngine* engine) : engine_(engine) {}
    bool execute(const ScanJobDesc& job, UnifiedScanResult& result) noexcept override;
};

#endif // AURORA_SCANNER_HANDLERS_HPP
