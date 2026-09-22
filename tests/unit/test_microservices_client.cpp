#include <gtest/gtest.h>
#include "../../services/service_registry.hpp"
#include "../../services/vfs/vfs_client.hpp"
#include "../../services/sensor/sensor_client.hpp"
#include "../../services/power/power_client.hpp"
#include "../../apps/elf_loader.hpp"
#include "../../apps/elf.hpp"
#include "../../vfs/vfs.hpp"
#include "../../vfs/ramfs.hpp"
#include <string.h>

using namespace auroraos::services;
using namespace auroraos::vfs;
using namespace auroraos::sensor_service;
using namespace auroraos::power_service;

TEST(ServiceRegistryTest, RegisterAndResolveServices) {
    ServiceRegistry& reg = ServiceRegistry::instance();
    reg.reset();
    EXPECT_EQ(reg.get_registered_count(), 0u);

    EXPECT_FALSE(reg.is_service_available(ServiceId::Vfs));
    EXPECT_EQ(reg.get_service_endpoint(ServiceId::Vfs), -1);

    // Register VFS and Net
    EXPECT_TRUE(reg.register_service(ServiceId::Vfs, 10, "VfsService"));
    EXPECT_TRUE(reg.register_service(ServiceId::Net, 11, "NetService"));
    EXPECT_EQ(reg.get_registered_count(), 2u);

    EXPECT_TRUE(reg.is_service_available(ServiceId::Vfs));
    EXPECT_EQ(reg.get_service_endpoint(ServiceId::Vfs), 10);
    EXPECT_STREQ(reg.get_service_name(ServiceId::Vfs), "VfsService");

    EXPECT_TRUE(reg.is_service_available(ServiceId::Net));
    EXPECT_EQ(reg.get_service_endpoint(ServiceId::Net), 11);
    EXPECT_STREQ(reg.get_service_name(ServiceId::Net), "NetService");

    // In-place update
    EXPECT_TRUE(reg.register_service(ServiceId::Vfs, 20, "VfsServiceV2"));
    EXPECT_EQ(reg.get_registered_count(), 2u);
    EXPECT_EQ(reg.get_service_endpoint(ServiceId::Vfs), 20);
    EXPECT_STREQ(reg.get_service_name(ServiceId::Vfs), "VfsServiceV2");

    // Unregister
    EXPECT_TRUE(reg.unregister_service(ServiceId::Vfs));
    EXPECT_FALSE(reg.is_service_available(ServiceId::Vfs));
    EXPECT_EQ(reg.get_service_endpoint(ServiceId::Vfs), -1);
    EXPECT_EQ(reg.get_registered_count(), 1u);

    reg.reset();
    EXPECT_EQ(reg.get_registered_count(), 0u);
}

// Simulated backend file for VfsClient tests
static char g_test_file_data[256];
static int g_test_file_len = 0;
static bool g_test_file_opened = false;

static void test_vfs_server_dispatcher(const VfsRequest& req, VfsReply& reply) {
    reply.status = -1;
    switch (req.opcode) {
    case VfsOpcode::Open:
        g_test_file_opened = true;
        reply.status = 3; // mock fd
        break;
    case VfsOpcode::Write:
        if (g_test_file_opened && req.fd == 3) {
            int write_len = req.write.len;
            if (write_len > static_cast<int>(sizeof(g_test_file_data))) {
                write_len = sizeof(g_test_file_data);
            }
            memcpy(g_test_file_data, req.write.data, write_len);
            g_test_file_len = write_len;
            reply.status = write_len;
        }
        break;
    case VfsOpcode::Read:
        if (g_test_file_opened && req.fd == 3) {
            int read_len = req.read.len;
            if (read_len > g_test_file_len) {
                read_len = g_test_file_len;
            }
            memcpy(reply.read.data, g_test_file_data, read_len);
            reply.status = read_len;
        }
        break;
    case VfsOpcode::Lseek:
        if (g_test_file_opened && req.fd == 3) {
            reply.status = req.lseek.offset;
        }
        break;
    case VfsOpcode::Close:
        if (g_test_file_opened && req.fd == 3) {
            g_test_file_opened = false;
            reply.status = 0;
        }
        break;
    default:
        break;
    }
}

TEST(VfsClientTest, ClientDispatchWorkflow) {
    VfsClient& client = VfsClient::instance();
    VfsClient::set_direct_handler(test_vfs_server_dispatcher);

    g_test_file_len = 0;
    g_test_file_opened = false;

    int fd = client.open("/data/test.txt", 0);
    EXPECT_EQ(fd, 3);
    EXPECT_TRUE(g_test_file_opened);

    const char* test_msg = "Hello Aurora Microservices!";
    int written = client.write(fd, test_msg, strlen(test_msg));
    EXPECT_EQ(written, static_cast<int>(strlen(test_msg)));

    char read_buf[64] = {0};
    int bytes_read = client.read(fd, read_buf, sizeof(read_buf));
    EXPECT_EQ(bytes_read, static_cast<int>(strlen(test_msg)));
    EXPECT_STREQ(read_buf, test_msg);

    EXPECT_EQ(client.lseek(fd, 0, 0), 0);
    EXPECT_EQ(client.close(fd), 0);
    EXPECT_FALSE(g_test_file_opened);

    // C-style wrapper test
    int fd2 = vfs_client_open("/data/wrap.txt", 0);
    EXPECT_EQ(fd2, 3);
    EXPECT_EQ(vfs_client_write(fd2, "CWrap", 5), 5);
    char wrap_buf[16] = {0};
    EXPECT_EQ(vfs_client_read(fd2, wrap_buf, 5), 5);
    EXPECT_STREQ(wrap_buf, "CWrap");
    EXPECT_EQ(vfs_client_close(fd2), 0);

    VfsClient::set_direct_handler(nullptr);
}

// Simulated backend for SensorClient
static uint32_t g_mock_bpm = 72;
static uint32_t g_mock_steps = 5432;
static bool g_sensor_subscribed = false;
static uint16_t g_sensor_rate = 0;

static void test_sensor_server_dispatcher(const SensorRequest& req, SensorReply& reply) {
    reply.status = 0;
    switch (req.opcode) {
    case SensorOpcode::Subscribe:
        g_sensor_subscribed = true;
        break;
    case SensorOpcode::Unsubscribe:
        g_sensor_subscribed = false;
        break;
    case SensorOpcode::SetSampleRate:
        g_sensor_rate = req.set_rate.rate_hz;
        break;
    case SensorOpcode::ReadLatest:
        reply.timestamp = 1000;
        if (req.read_latest.type == IpcSensorType::HEART_RATE) {
            reply.data.bpm = g_mock_bpm;
        } else if (req.read_latest.type == IpcSensorType::STEP_COUNTER) {
            reply.data.steps = g_mock_steps;
        } else {
            reply.status = -1;
        }
        break;
    }
}

TEST(SensorClientTest, ClientDispatchWorkflow) {
    SensorClient& client = SensorClient::instance();
    SensorClient::set_direct_handler(test_sensor_server_dispatcher);

    EXPECT_TRUE(client.subscribe(IpcSensorType::HEART_RATE));
    EXPECT_TRUE(g_sensor_subscribed);

    EXPECT_TRUE(client.set_sample_rate(IpcSensorType::HEART_RATE, 25));
    EXPECT_EQ(g_sensor_rate, 25);

    EXPECT_EQ(client.read_heart_rate(), 72u);
    EXPECT_EQ(client.read_steps(), 5432u);

    EXPECT_TRUE(client.unsubscribe(IpcSensorType::HEART_RATE));
    EXPECT_FALSE(g_sensor_subscribed);

    SensorClient::set_direct_handler(nullptr);
}

// Simulated backend for PowerClient
static bool g_mock_wakelock = false;
static uint8_t g_mock_battery = 88;
static PowerState g_mock_power_state = PowerState::RUN;

static void test_power_server_dispatcher(const PowerRequest& req, PowerReply& reply) {
    reply.status = 0;
    switch (req.opcode) {
    case PowerOpcode::AcquireWakeLock:
        g_mock_wakelock = true;
        break;
    case PowerOpcode::ReleaseWakeLock:
        g_mock_wakelock = false;
        break;
    case PowerOpcode::GetBatteryLevel:
        reply.data.battery_percent = g_mock_battery;
        break;
    case PowerOpcode::GetPowerState:
        reply.data.current_state = g_mock_power_state;
        break;
    }
}

TEST(PowerClientTest, ClientDispatchWorkflow) {
    PowerClient& client = PowerClient::instance();
    PowerClient::set_direct_handler(test_power_server_dispatcher);

    EXPECT_TRUE(client.acquire_wake_lock());
    EXPECT_TRUE(g_mock_wakelock);

    EXPECT_EQ(client.get_battery_level(), 88);

    g_mock_power_state = PowerState::LIGHT_SLEEP;
    EXPECT_EQ(client.get_power_state(), PowerState::LIGHT_SLEEP);

    EXPECT_TRUE(client.release_wake_lock());
    EXPECT_FALSE(g_mock_wakelock);

    PowerClient::set_direct_handler(nullptr);
}

TEST(ElfLoaderManifestTest, ReadEmbeddedManifestFromElf) {
    VfsManager::instance().init();
    static RamFile ramfile(8192);
    const char* filepath = "/manifest_test.elf";
    VfsManager::instance().mount(filepath, &ramfile);

    // Construct an ELF binary with .shstrtab and .aurora_manifest
    Elf32_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = ELFMAG0;
    ehdr.e_ident[1] = ELFMAG1;
    ehdr.e_ident[2] = ELFMAG2;
    ehdr.e_ident[3] = ELFMAG3;
    ehdr.e_type = 2; // ET_EXEC
    ehdr.e_machine = EM_ARM;
    ehdr.e_version = 1;
    ehdr.e_entry = 0x20000000;
    ehdr.e_phoff = sizeof(Elf32_Ehdr);
    ehdr.e_phentsize = sizeof(Elf32_Phdr);
    ehdr.e_phnum = 1;

    // We create 3 sections: 0=NULL, 1=.shstrtab, 2=.aurora_manifest
    ehdr.e_shoff = sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr) + 64; // past program header and payload
    ehdr.e_shentsize = sizeof(Elf32_Shdr);
    ehdr.e_shnum = 3;
    ehdr.e_shstrndx = 1;

    // 1 Program Header (loadable code)
    Elf32_Phdr phdr;
    memset(&phdr, 0, sizeof(phdr));
    phdr.p_type = PT_LOAD;
    phdr.p_offset = sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr);
    phdr.p_vaddr = 0x20000000;
    phdr.p_paddr = 0x20000000;
    phdr.p_filesz = 64;
    phdr.p_memsz = 64;
    phdr.p_flags = PF_R | PF_X; // Read + Execute (Compliant W^X)
    phdr.p_align = 4;

    uint8_t payload[64] = {0};

    // Shstrtab data: "\0.shstrtab\0.aurora_manifest\0"
    const char shstrtab_data[] = "\0.shstrtab\0.aurora_manifest\0";
    uint32_t shstrtab_offset = ehdr.e_shoff + 3 * sizeof(Elf32_Shdr);
    uint32_t shstrtab_size = sizeof(shstrtab_data);

    // Manifest data
    auroraos::runtime::AppManifest test_manifest{};
    strncpy(test_manifest.name, "ElfTestApp", sizeof(test_manifest.name) - 1);
    test_manifest.required_caps = static_cast<uint32_t>(auroraos::runtime::AppCapability::Sensor | auroraos::runtime::AppCapability::Network);
    test_manifest.max_memory_bytes = 16384;
    test_manifest.max_cpu_percent = 40;
    test_manifest.priority = 8;
    test_manifest.stack_size_pow2 = 12;

    uint32_t manifest_offset = shstrtab_offset + shstrtab_size;
    uint32_t manifest_size = sizeof(test_manifest);

    // Section Headers
    Elf32_Shdr shdrs[3];
    memset(shdrs, 0, sizeof(shdrs));

    // Section 1: .shstrtab (offset 1 in shstrtab_data is ".shstrtab")
    shdrs[1].sh_name = 1;
    shdrs[1].sh_type = SHT_STRTAB;
    shdrs[1].sh_offset = shstrtab_offset;
    shdrs[1].sh_size = shstrtab_size;

    // Section 2: .aurora_manifest (offset 11 in shstrtab_data is ".aurora_manifest")
    shdrs[2].sh_name = 11;
    shdrs[2].sh_type = SHT_PROGBITS;
    shdrs[2].sh_offset = manifest_offset;
    shdrs[2].sh_size = manifest_size;

    // Write file to RAMFS
    int fd = VfsManager::instance().open(filepath);
    ASSERT_GE(fd, 0);
    VfsManager::instance().write(fd, reinterpret_cast<const char*>(&ehdr), sizeof(ehdr));
    VfsManager::instance().write(fd, reinterpret_cast<const char*>(&phdr), sizeof(phdr));
    VfsManager::instance().write(fd, reinterpret_cast<const char*>(payload), sizeof(payload));
    VfsManager::instance().lseek(fd, ehdr.e_shoff, 0);
    VfsManager::instance().write(fd, reinterpret_cast<const char*>(shdrs), sizeof(shdrs));
    VfsManager::instance().lseek(fd, shstrtab_offset, 0);
    VfsManager::instance().write(fd, shstrtab_data, shstrtab_size);
    VfsManager::instance().lseek(fd, manifest_offset, 0);
    VfsManager::instance().write(fd, reinterpret_cast<const char*>(&test_manifest), manifest_size);
    VfsManager::instance().close(fd);

    // Verify ElfLoader::read_manifest
    auroraos::runtime::AppManifest extracted_manifest{};
    bool read_ok = ElfLoader::read_manifest(filepath, extracted_manifest);
    EXPECT_TRUE(read_ok);
    EXPECT_TRUE(extracted_manifest.is_valid());
    EXPECT_STREQ(extracted_manifest.name, "ElfTestApp");
    EXPECT_TRUE(extracted_manifest.has_capability(auroraos::runtime::AppCapability::Sensor));
    EXPECT_TRUE(extracted_manifest.has_capability(auroraos::runtime::AppCapability::Network));
    EXPECT_FALSE(extracted_manifest.has_capability(auroraos::runtime::AppCapability::Timer));
    EXPECT_EQ(extracted_manifest.max_memory_bytes, 16384u);
    EXPECT_EQ(extracted_manifest.max_cpu_percent, 40u);
}
