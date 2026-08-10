#include <gtest/gtest.h>
#include "drivers/sensor/sensor_framework.hpp"

class SensorFrameworkTest : public ::testing::Test {
protected:
    void SetUp() override {
        SensorManager::instance().init_all();
    }
};

TEST_F(SensorFrameworkTest, HeartRateRead) {
    auto& hr = SensorManager::instance().get_hr_sensor();
    hr.power_up();
    
    SensorData data;
    EXPECT_TRUE(hr.read(&data));
    EXPECT_EQ(data.type, SensorType::HEART_RATE);
    EXPECT_GT(data.payload.bpm, 0); // Should return simulated bpm
}

TEST_F(SensorFrameworkTest, AccelerometerStepDetection) {
    auto& accel = SensorManager::instance().get_accel_sensor();
    accel.power_up();
    
    uint32_t initial_steps = accel.get_steps();
    
    SensorData data;
    
    // Simulate STABLE (1g = 1000mg)
    accel.set_mock_data(0, 0, 1000);
    accel.read(&data);
    
    // Simulate RISING (lift leg > 1200)
    accel.set_mock_data(0, 0, 1300);
    accel.read(&data);
    
    // Simulate FALLING (drop leg < 800)
    accel.set_mock_data(0, 0, 700);
    accel.read(&data);
    
    // Simulate return to STABLE (900-1100)
    accel.set_mock_data(0, 0, 1000);
    accel.read(&data);
    
    EXPECT_EQ(accel.get_steps(), initial_steps + 1);
}
