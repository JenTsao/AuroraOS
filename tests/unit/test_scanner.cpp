// =============================================================================
// test_scanner.cpp — Unit tests for Network Scanner Subsystem
// =============================================================================

#include <gtest/gtest.h>
#include "../../net/scanner/port_scanner.hpp"
#include "../../net/scanner/service_detector.hpp"
#include "../../net/scanner/vuln_probe.hpp"

// ---- Definitions of static tables and stubs for host test ----

const ServiceFingerprint ServiceDetector::fingerprints_[ServiceDetector::fingerprint_count_] = {
    {"openssh", 22, nullptr, "ssh", false},
    {"dropbear", 22, nullptr, "dropbear", false},
    {"proftpd", 21, nullptr, "proftpd", false},
    {"vsftpd", 21, nullptr, "vsftpd", false},
    {"pure-ftpd", 21, nullptr, "pure-ftpd", false},
    {"postfix", 25, nullptr, "postfix", false},
    {"sendmail", 25, nullptr, "sendmail", false},
    {"exim", 25, nullptr, "exim", false},
    {"dovecot", 110, nullptr, "dovecot", false},
    {"dovecot", 143, nullptr, "dovecot", false},
    {"courier", 110, nullptr, "courier-imap", false},
    {"mysql", 3306, nullptr, "mysql", false},
    {"mariadb", 3306, nullptr, "mariadb", false},
    {"postgresql", 5432, nullptr, "postgresql", false},
    {"redis", 6379, nullptr, "redis", false},
    {"mongodb", 27017, nullptr, "mongodb", false},
    {"nginx", 80, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n", "nginx", false},
    {"apache", 80, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n", "apache", false},
    {"iis", 80, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n", "microsoft", false},
    {"tomcat", 8080, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n", "tomcat", false},
    {"jetty", 8080, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n", "jetty", false},
    {"node.js", 8080, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n", "node", false},
};

const CveSignature VulnProbe::cve_signatures_[VulnProbe::cve_count_] = {
    {"CVE-2014-0160", "openssl", "1.0.1", "\x18\x03\x02\x00\x03\x01\x40\x00", 8, "heartbeat", Severity::Critical,
     "Heartbleed - OpenSSL TLS heartbeat read overrun", 443},
    {"CVE-2017-7494", "samba", "3.5.0", nullptr, 0, "samba", Severity::Critical,
     "SambaCry - Remote code execution via shared library upload", 445},
    {"CVE-2019-0708", "rdp", "windows", nullptr, 0, "rdp", Severity::Critical,
     "BlueKeep - Windows RDP remote code execution", 3389},
    {"CVE-2021-41773", "apache", "2.4.49",
     "GET /cgi-bin/.%2e/%2e%2e/%2e%2e/etc/passwd HTTP/1.0\r\nHost: localhost\r\n\r\n", 68, "root:", Severity::High,
     "Apache HTTP Server path traversal and file disclosure", 80},
    {"CVE-2021-44228", "log4j", "2.0", "GET / HTTP/1.0\r\nUser-Agent: ${jndi:ldap://probe}\r\nHost: localhost\r\n\r\n",
     67, "log4j", Severity::Critical, "Log4Shell - Apache Log4j2 JNDI remote code execution", 8080},
    {"CVE-2022-22965", "spring", "5.3.0",
     "GET /?class.module.classLoader.resources.context.parent.pipeline.first.pattern= HTTP/1.0\r\nHost: "
     "localhost\r\n\r\n",
     89, "spring", Severity::Critical, "Spring4Shell - Spring Framework RCE via data binding", 8080},
    {"CVE-2023-44487", "http", "*", nullptr, 0, "rst_stream", Severity::High, "HTTP/2 Rapid Reset denial of service",
     443},
    {"CVE-2018-15473", "openssh", "7.7", nullptr, 0, "openssh", Severity::Medium,
     "OpenSSH user enumeration via malformed auth request", 22},
    {"CVE-2019-15107", "webmin", "1.8",
     "POST /password_change.cgi HTTP/1.0\r\nHost: localhost\r\nReferer: localhost\r\nContent-Length: "
     "48\r\n\r\nuser=root&pam=&expired=2&old=test|id&new1=new&new2=new",
     141, "uid=", Severity::Critical, "Webmin remote command injection via password change", 10000},
    {"CVE-2022-1388", "f5", "big-ip",
     "POST /mgmt/tm/util/bash HTTP/1.0\r\nHost: localhost\r\nX-F5-Auth-Token: bypass\r\nConnection: keep-alive\r\n\r\n",
     98, "commandResult", Severity::Critical, "F5 BIG-IP iControl REST authentication bypass", 443},
    {"CVE-2021-21972", "vmware", "vcenter", nullptr, 0, "vmware", Severity::Critical,
     "VMware vCenter Server unauthenticated file upload RCE", 443},
    {"CVE-2023-34362", "progress", "moveit", nullptr, 0, "moveit", Severity::Critical,
     "MOVEit Transfer SQL injection leading to RCE", 443},
};

void ServiceDetector::yield_cpu_() {}
void VulnProbe::yield_cpu_() {}
uint32_t PortScanner::get_tick_count_() { return 0; }
void PortScanner::yield_cpu_() {}

// -----------------------------------------------------------------------------
// Test PortScanner Profiles, Inter-packet Delays, and Port Shuffling
// -----------------------------------------------------------------------------

TEST(PortScannerTest, ProfilesAndTimingSettings) {
    PortScanner scanner;

    // Default profile is NORMAL
    EXPECT_EQ(scanner.get_profile(), ScanProfile::NORMAL);
    EXPECT_EQ(scanner.get_inter_packet_delay(), 0u);

    // Set PARANOID
    scanner.set_profile(ScanProfile::PARANOID);
    EXPECT_EQ(scanner.get_profile(), ScanProfile::PARANOID);
    EXPECT_EQ(scanner.get_inter_packet_delay(), 300u);

    // Set SNEAKY
    scanner.set_profile(ScanProfile::SNEAKY);
    EXPECT_EQ(scanner.get_profile(), ScanProfile::SNEAKY);
    EXPECT_EQ(scanner.get_inter_packet_delay(), 100u);

    // Set AGGRESSIVE
    scanner.set_profile(ScanProfile::AGGRESSIVE);
    EXPECT_EQ(scanner.get_profile(), ScanProfile::AGGRESSIVE);
    EXPECT_EQ(scanner.get_inter_packet_delay(), 0u);

    // Set INSANE
    scanner.set_profile(ScanProfile::INSANE);
    EXPECT_EQ(scanner.get_profile(), ScanProfile::INSANE);

    // Custom delay
    scanner.set_inter_packet_delay(50);
    EXPECT_EQ(scanner.get_inter_packet_delay(), 50u);
}

TEST(PortScannerTest, StateAndTypeStrings) {
    EXPECT_STREQ(PortScanner::port_state_to_string(PortState::Open), "open");
    EXPECT_STREQ(PortScanner::port_state_to_string(PortState::Closed), "closed");
    EXPECT_STREQ(PortScanner::port_state_to_string(PortState::Filtered), "filtered");
    EXPECT_STREQ(PortScanner::port_state_to_string(PortState::Error), "error");

    EXPECT_STREQ(PortScanner::scan_type_to_string(ScanType::TcpConnect), "tcp_connect");
    EXPECT_STREQ(PortScanner::scan_type_to_string(ScanType::Udp), "udp");
    EXPECT_STREQ(PortScanner::scan_type_to_string(ScanType::Ack), "ack");
}

TEST(PortScannerTest, PortShuffling) {
    uint16_t original_ports[] = {21, 22, 23, 25, 53, 80, 443, 3306, 8080};
    constexpr int kCount = sizeof(original_ports) / sizeof(original_ports[0]);

    uint16_t shuffled_ports[kCount];
    memcpy(shuffled_ports, original_ports, sizeof(original_ports));

    // Shuffle with seed
    PortScanner::shuffle_ports(shuffled_ports, kCount, 987654321);

    // All original elements must still exist
    for (int i = 0; i < kCount; ++i) {
        bool found = false;
        for (int j = 0; j < kCount; ++j) {
            if (shuffled_ports[j] == original_ports[i]) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found);
    }

    // Must be shuffled (not identical order)
    bool differs = false;
    for (int i = 0; i < kCount; ++i) {
        if (shuffled_ports[i] != original_ports[i]) {
            differs = true;
            break;
        }
    }
    EXPECT_TRUE(differs);
}

// -----------------------------------------------------------------------------
// Test ServiceDetector Fingerprint Identification
// -----------------------------------------------------------------------------

TEST(ServiceDetectorTest, PortBasedIdentification) {
    ServiceDetector detector;
    ServiceInfo info{};

    info.port = 22;
    info.banner_len = 0;
    EXPECT_TRUE(detector.identify_service(info));
    EXPECT_TRUE(info.identified);
    EXPECT_STREQ(info.service, "openssh");

    info.port = 3306;
    info.banner_len = 0;
    EXPECT_TRUE(detector.identify_service(info));
    EXPECT_STREQ(info.service, "mysql");

    info.port = 6379;
    info.banner_len = 0;
    EXPECT_TRUE(detector.identify_service(info));
    EXPECT_STREQ(info.service, "redis");
}

// -----------------------------------------------------------------------------
// Test VulnProbe Severity and Signatures
// -----------------------------------------------------------------------------

TEST(VulnProbeTest, SeverityToString) {
    EXPECT_STREQ(VulnProbe::severity_to_string(Severity::Critical), "critical");
    EXPECT_STREQ(VulnProbe::severity_to_string(Severity::High), "high");
    EXPECT_STREQ(VulnProbe::severity_to_string(Severity::Medium), "medium");
    EXPECT_STREQ(VulnProbe::severity_to_string(Severity::Low), "low");
    EXPECT_STREQ(VulnProbe::severity_to_string(Severity::Info), "info");
}
