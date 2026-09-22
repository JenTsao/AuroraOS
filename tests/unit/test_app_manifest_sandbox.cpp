#include <gtest/gtest.h>
#include "../../runtime/app_manifest.hpp"
#include "../../runtime/app_sandbox.hpp"
#include "../../runtime/app_base.hpp"
#include "../../runtime/aurora_runtime.hpp"
#include <string.h>

using namespace auroraos::runtime;

TEST(AppManifestTest, DefaultInitializationAndValidation) {
    AppManifest manifest{};
    EXPECT_EQ(manifest.magic, AURORA_MANIFEST_MAGIC);
    EXPECT_EQ(manifest.manifest_version, AURORA_MANIFEST_VERSION);
    EXPECT_FALSE(manifest.is_valid()); // Name is empty by default

    strncpy(manifest.name, "TestApp", sizeof(manifest.name) - 1);
    manifest.max_cpu_percent = 50;
    EXPECT_TRUE(manifest.is_valid());

    // Invalid magic
    manifest.magic = 0x12345678;
    EXPECT_FALSE(manifest.is_valid());
    manifest.magic = AURORA_MANIFEST_MAGIC;

    // Invalid version
    manifest.manifest_version = 999;
    EXPECT_FALSE(manifest.is_valid());
    manifest.manifest_version = AURORA_MANIFEST_VERSION;

    // Invalid CPU percent (> 100)
    manifest.max_cpu_percent = 101;
    EXPECT_FALSE(manifest.is_valid());
}

TEST(AppManifestTest, CapabilityBitmaskOperators) {
    AppCapability cap1 = AppCapability::Network;
    AppCapability cap2 = AppCapability::FileSystem;
    AppCapability combined = cap1 | cap2;

    EXPECT_TRUE(static_cast<uint32_t>(combined & AppCapability::Network) != 0);
    EXPECT_TRUE(static_cast<uint32_t>(combined & AppCapability::FileSystem) != 0);
    EXPECT_FALSE(static_cast<uint32_t>(combined & AppCapability::Sensor) != 0);

    AppManifest manifest{};
    strncpy(manifest.name, "NetApp", sizeof(manifest.name) - 1);
    manifest.required_caps = static_cast<uint32_t>(combined);

    EXPECT_TRUE(manifest.has_capability(AppCapability::Network));
    EXPECT_TRUE(manifest.has_capability(AppCapability::FileSystem));
    EXPECT_FALSE(manifest.has_capability(AppCapability::Sensor));
    EXPECT_FALSE(manifest.has_capability(AppCapability::Timer));
}

TEST(AppSandboxTest, CapabilityGatingAndModification) {
    AppManifest manifest{};
    strncpy(manifest.name, "GatedApp", sizeof(manifest.name) - 1);
    manifest.required_caps = static_cast<uint32_t>(AppCapability::Sensor | AppCapability::Timer);

    AppSandbox sandbox(manifest);
    EXPECT_TRUE(sandbox.has_capability(AppCapability::Sensor));
    EXPECT_TRUE(sandbox.has_capability(AppCapability::Timer));
    EXPECT_FALSE(sandbox.has_capability(AppCapability::Network));

    // Dynamic grant
    EXPECT_TRUE(sandbox.grant_capability(AppCapability::Network));
    EXPECT_TRUE(sandbox.has_capability(AppCapability::Network));

    // Dynamic revoke
    EXPECT_TRUE(sandbox.revoke_capability(AppCapability::Sensor));
    EXPECT_FALSE(sandbox.has_capability(AppCapability::Sensor));
}

TEST(AppSandboxTest, MemoryQuotaEnforcementAndUnderflowProtection) {
    AppManifest manifest{};
    strncpy(manifest.name, "MemApp", sizeof(manifest.name) - 1);
    manifest.max_memory_bytes = 1024; // 1KB limit

    AppSandbox sandbox(manifest);
    EXPECT_EQ(sandbox.get_memory_usage(), 0u);
    EXPECT_EQ(sandbox.get_memory_limit(), 1024u);

    // Allocation within limit
    EXPECT_TRUE(sandbox.allocate_memory(512));
    EXPECT_EQ(sandbox.get_memory_usage(), 512u);
    EXPECT_EQ(sandbox.get_status(), SandboxStatus::Active);

    // Allocation exceeding limit
    EXPECT_FALSE(sandbox.allocate_memory(600)); // 512 + 600 = 1112 > 1024
    EXPECT_EQ(sandbox.get_memory_usage(), 512u);
    EXPECT_EQ(sandbox.get_status(), SandboxStatus::Violation);
    EXPECT_EQ(sandbox.get_violation_count(), 1u);
    EXPECT_STREQ(sandbox.get_violation_reason(), "Memory quota exceeded");

    // Free memory with underflow protection
    sandbox.free_memory(300);
    EXPECT_EQ(sandbox.get_memory_usage(), 212u);

    sandbox.free_memory(500); // Exceeds current usage, must saturate at 0
    EXPECT_EQ(sandbox.get_memory_usage(), 0u);

    // Reset violation
    sandbox.reset_violation();
    EXPECT_EQ(sandbox.get_status(), SandboxStatus::Active);
}

TEST(AppSandboxTest, CpuQuotaAndTaskBinding) {
    AppManifest manifest{};
    strncpy(manifest.name, "CpuApp", sizeof(manifest.name) - 1);
    manifest.max_cpu_percent = 30;

    AppSandbox sandbox(manifest);
    EXPECT_EQ(sandbox.get_cpu_limit(), 30u);
    EXPECT_TRUE(sandbox.check_cpu_quota(25));
    EXPECT_TRUE(sandbox.check_cpu_quota(30));
    EXPECT_FALSE(sandbox.check_cpu_quota(35));

    // Task ID binding
    sandbox.set_task_id(42);
    EXPECT_EQ(sandbox.get_task_id(), 42u);
}

class TestCustomApp : public AppBase {
public:
    explicit TestCustomApp(const AppManifest& manifest) : AppBase(manifest), started_(false), stopped_(false) {}

    bool started_;
    bool stopped_;

protected:
    bool on_start() override {
        started_ = true;
        return true;
    }

    void on_stop() override {
        stopped_ = true;
    }
};

TEST(AuroraRuntimeTest, RegistrationAndLifecycleManagement) {
    AuroraRuntime& runtime = AuroraRuntime::instance();
    runtime.reset();
    EXPECT_EQ(runtime.get_app_count(), 0);

    AppManifest manifest1{};
    strncpy(manifest1.name, "AppOne", sizeof(manifest1.name) - 1);
    manifest1.max_memory_bytes = 4096;
    TestCustomApp app1(manifest1);

    AppManifest manifest2{};
    strncpy(manifest2.name, "AppTwo", sizeof(manifest2.name) - 1);
    manifest2.max_memory_bytes = 2048;
    TestCustomApp app2(manifest2);

    EXPECT_TRUE(runtime.register_app(&app1));
    EXPECT_TRUE(runtime.register_app(&app2));
    EXPECT_EQ(runtime.get_app_count(), 2);

    // Duplicate registration is idempotent
    EXPECT_TRUE(runtime.register_app(&app1));
    EXPECT_EQ(runtime.get_app_count(), 2);

    EXPECT_EQ(runtime.get_app_by_name("AppOne"), &app1);
    EXPECT_EQ(runtime.get_app_by_name("AppTwo"), &app2);
    EXPECT_EQ(runtime.get_app_by_name("NonExistent"), nullptr);

    EXPECT_EQ(runtime.get_app_by_index(0), &app1);
    EXPECT_EQ(runtime.get_app_by_index(1), &app2);
    EXPECT_EQ(runtime.get_app_by_index(2), nullptr);

    // Lifecycle: Start all
    runtime.start_all();
    EXPECT_TRUE(app1.started_);
    EXPECT_TRUE(app2.started_);
    EXPECT_EQ(app1.get_state(), AppState::Running);
    EXPECT_EQ(app2.get_state(), AppState::Running);

    // Audit with violation: If app1 violates quota, audit_apps should stop it
    app1.get_sandbox().record_violation("Testing audit");
    runtime.audit_apps();
    EXPECT_EQ(app1.get_state(), AppState::Stopped);
    EXPECT_TRUE(app1.stopped_);

    // Lifecycle: Stop all
    runtime.stop_all();
    EXPECT_EQ(app2.get_state(), AppState::Stopped);

    // Unregister
    EXPECT_TRUE(runtime.unregister_app(&app1));
    EXPECT_EQ(runtime.get_app_count(), 1);
    EXPECT_EQ(runtime.get_app_by_name("AppOne"), nullptr);

    runtime.reset();
    EXPECT_EQ(runtime.get_app_count(), 0);
}
