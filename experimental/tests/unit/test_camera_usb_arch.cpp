#include <gtest/gtest.h>
#include "../../drivers/usb/usb_core.hpp"
#include "../../apps/camera_app.cpp" 
#include "../../drivers/gpu/soft_gpu_device.hpp"
#include "../../guix/compositor.hpp"

using namespace auroraos::usb;
using namespace auroraos::camera;
using namespace auroraos::gpu;
using namespace auroraos::guix;
using namespace auroraos::apps;

TEST(UsbCoreTest, SetupPacketSize) {
    // USB standard requires exactly 8 bytes for a setup packet
    EXPECT_EQ(sizeof(UsbSetupPacket), 8);
}

TEST(CameraAppIntegrationTest, FrameToWindowPipeline) {
    SoftGpuDevice gpu;
    Surface screen(800, 600);
    Compositor compositor(&screen, &gpu);
    
    // Instantiate the camera app (this automatically creates a window and starts the dummy camera)
    CameraApp app(&compositor, &gpu);
    
    // Simulate 3 frames arriving (16ms ~ 60fps scheduler tick)
    app.tick(16);
    app.tick(16);
    app.tick(16);
    
    // The camera app should have requested invalidation.
    // We now trigger the compositor to actually render the screen.
    compositor.composite();
    
    // Check if the camera frame was blitted to the screen surface
    uint16_t* screen_buf = static_cast<uint16_t*>(screen.get_buffer());
    
    // Window was moved to (50, 50). 
    // We just check that the pixel isn't the default black (0x0000). 
    // It proves the pipeline: Camera -> Surface -> Window -> Compositor -> Screen worked!
    EXPECT_NE(screen_buf[55 * 800 + 55], 0x0000);
}
