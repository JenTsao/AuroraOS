#include <gtest/gtest.h>
#include "../../services/service_manager.hpp"
#include "../../services/service_registry.hpp"
#include "../../services/vfs/vfs_service.hpp"
#include "../../services/vfs/vfs_client.hpp"
#include "../../services/net/net_service.hpp"
#include "../../services/firewall/firewall_service.hpp"
#include "../../services/firewall/firewall_client.hpp"
#include "../../services/sensor/sensor_service.hpp"
#include "../../services/sensor/sensor_client.hpp"
#include "../../services/power/power_service.hpp"
#include "../../services/power/power_client.hpp"
#include "../../services/display/display_service.hpp"
#include "../../services/display/display_client.hpp"
#include <string.h>

using namespace auroraos::services;
using namespace auroraos::vfs;
using namespace auroraos::net;
using namespace auroraos::firewall;
using namespace auroraos::sensor_service;
using namespace auroraos::power_service;
using namespace auroraos::display;

TEST(ServiceManagerTest, LifecycleAndHealthCheck) {
    ServiceManager& mgr = ServiceManager::instance();
    mgr.init_all_services();

    EXPECT_EQ(mgr.get_active_service_count(), 6u);
    EXPECT_TRUE(mgr.health_check());

    EXPECT_EQ(mgr.get_service_state(ServiceId::Vfs), ServiceState::Running);
    EXPECT_EQ(mgr.get_service_state(ServiceId::Net), ServiceState::Running);
    EXPECT_EQ(mgr.get_service_state(ServiceId::Firewall), ServiceState::Running);
    EXPECT_EQ(mgr.get_service_state(ServiceId::Sensor), ServiceState::Running);
    EXPECT_EQ(mgr.get_service_state(ServiceId::Power), ServiceState::Running);
    EXPECT_EQ(mgr.get_service_state(ServiceId::UI), ServiceState::Running);

    // Stop a service
    EXPECT_TRUE(mgr.stop_service(ServiceId::Sensor));
    EXPECT_EQ(mgr.get_service_state(ServiceId::Sensor), ServiceState::Stopped);
    EXPECT_EQ(mgr.get_active_service_count(), 5u);
    EXPECT_FALSE(mgr.health_check()); // A service is stopped, health_check must fail

    // Restart the service
    EXPECT_TRUE(mgr.restart_service(ServiceId::Sensor));
    EXPECT_EQ(mgr.get_service_state(ServiceId::Sensor), ServiceState::Running);
    EXPECT_TRUE(mgr.health_check());

    const ServiceDescriptor* desc = mgr.get_service_descriptor(ServiceId::Sensor);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->restart_count, 1u);
    EXPECT_STREQ(desc->name, "SensorService");
}

TEST(ServerProcessingTest, SensorServerDirectProcessing) {
    SensorServer& server = SensorServer::instance();
    server.init();
    server.set_mock_data(82, 6789, 10, 20, 980);

    SensorRequest req;
    SensorReply reply;

    // Subscribe
    req.opcode = SensorOpcode::Subscribe;
    server.process_request(req, reply);
    EXPECT_EQ(reply.status, 0);
    EXPECT_TRUE(server.is_subscribed());

    // Set sample rate
    req.opcode = SensorOpcode::SetSampleRate;
    req.set_rate.rate_hz = 50;
    server.process_request(req, reply);
    EXPECT_EQ(reply.status, 0);
    EXPECT_EQ(server.get_sample_rate(), 50);

    // Read heart rate
    req.opcode = SensorOpcode::ReadLatest;
    req.read_latest.type = IpcSensorType::HEART_RATE;
    server.process_request(req, reply);
    EXPECT_EQ(reply.status, 0);
    EXPECT_EQ(reply.data.bpm, 82u);

    // Read steps
    req.read_latest.type = IpcSensorType::STEP_COUNTER;
    server.process_request(req, reply);
    EXPECT_EQ(reply.status, 0);
    EXPECT_EQ(reply.data.steps, 6789u);

    // Read accelerometer
    req.read_latest.type = IpcSensorType::ACCELEROMETER;
    server.process_request(req, reply);
    EXPECT_EQ(reply.status, 0);
    EXPECT_EQ(reply.data.accel.x, 10);
    EXPECT_EQ(reply.data.accel.y, 20);
    EXPECT_EQ(reply.data.accel.z, 980);
}

TEST(ServerProcessingTest, PowerServerDirectProcessing) {
    PowerServer& server = PowerServer::instance();
    server.init();

    PowerRequest req;
    PowerReply reply;

    // WakeLock acquire & release
    req.opcode = PowerOpcode::AcquireWakeLock;
    server.process_request(req, reply, 1);
    EXPECT_EQ(reply.status, 0);

    req.opcode = PowerOpcode::ReleaseWakeLock;
    server.process_request(req, reply, 1);
    EXPECT_EQ(reply.status, 0);

    // Battery level
    req.opcode = PowerOpcode::GetBatteryLevel;
    server.process_request(req, reply);
    EXPECT_EQ(reply.status, 0);
    EXPECT_LE(reply.data.battery_percent, 100);

    // Power state
    req.opcode = PowerOpcode::GetPowerState;
    server.process_request(req, reply);
    EXPECT_EQ(reply.status, 0);
}

TEST(ServerProcessingTest, FirewallServerDirectProcessing) {
    FirewallServer& server = FirewallServer::instance();
    server.init();

    FirewallRequest req;
    FirewallReply reply;

    // Packet processing with valid dummy packet
    req.opcode = FirewallOpcode::ProcessPacket;
    req.packet.len = 64;
    strncpy(req.packet.interface_name, "wlan0", sizeof(req.packet.interface_name));
    memset(req.packet.payload, 0, 64);
    req.packet.payload[12] = 0x08; // IPv4
    req.packet.payload[14] = 0x45; // IPv4 header

    server.process_request(req, reply);
    EXPECT_EQ(reply.status, 0); // Accepted by default

    // Rules and stats
    req.opcode = FirewallOpcode::AddRule;
    server.process_request(req, reply);
    EXPECT_EQ(reply.status, 0);

    req.opcode = FirewallOpcode::GetStats;
    server.process_request(req, reply);
    EXPECT_EQ(reply.status, 0);
}

static void direct_display_dispatcher(const DisplayRequest& req, DisplayReply& reply) {
    DisplayServer::instance().process_request(req, reply);
}

TEST(DisplayServiceTest, ServerAndClientDrawingWorkflow) {
    DisplayServer& server = DisplayServer::instance();
    server.init();

    DisplayClient& client = DisplayClient::instance();
    DisplayClient::set_direct_handler(direct_display_dispatcher);

    // 1. Get info
    DisplayInfoReply info{};
    EXPECT_TRUE(client.get_info(info));
    EXPECT_EQ(info.width, 128);
    EXPECT_EQ(info.height, 64);
    EXPECT_EQ(info.bpp, 16);

    // 2. Clear to black
    EXPECT_TRUE(client.clear(0x0000));
    EXPECT_EQ(server.get_pixel(10, 10), 0x0000);

    // 3. Draw pixel
    EXPECT_TRUE(client.draw_pixel(15, 20, 0xF800)); // Red in RGB565
    EXPECT_EQ(server.get_pixel(15, 20), 0xF800);
    EXPECT_EQ(server.get_pixel(15, 21), 0x0000);

    // 4. Fill rect
    EXPECT_TRUE(client.fill_rect(30, 40, 10, 5, 0x07E0)); // Green
    EXPECT_EQ(server.get_pixel(30, 40), 0x07E0);
    EXPECT_EQ(server.get_pixel(39, 44), 0x07E0);
    EXPECT_EQ(server.get_pixel(40, 40), 0x0000);

    // 5. Draw horizontal line
    EXPECT_TRUE(client.draw_line(0, 5, 10, 5, 0x001F)); // Blue
    for (int16_t x = 0; x <= 10; ++x) {
        EXPECT_EQ(server.get_pixel(x, 5), 0x001F);
    }

    // 6. Present
    uint32_t count_before = server.get_present_count();
    EXPECT_TRUE(client.present());
    EXPECT_EQ(server.get_present_count(), count_before + 1);

    DisplayClient::set_direct_handler(nullptr);
}
