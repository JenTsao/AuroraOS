#include <gtest/gtest.h>
#include "../../guix/window.hpp"
#include "../../guix/compositor.hpp"
#include "../../drivers/gpu/soft_gpu_device.hpp"

using namespace auroraos::guix;
using namespace auroraos::gpu;

TEST(GuixCompositorTest, BasicComposition) {
    // 1. Setup GPU and Screen
    SoftGpuDevice gpu;
    Surface screen(800, 600);
    Compositor compositor(&screen, &gpu);

    // 2. Create Window A (Background)
    Window winA(400, 300, &gpu, &compositor);
    winA.move(100, 100);
    winA.set_z_order(0);
    winA.fill_rect(0, 0, 400, 300, 0x1111); // Fill with color 0x1111

    // 3. Create Window B (Foreground)
    Window winB(200, 150, &gpu, &compositor);
    winB.move(200, 200); // Overlaps with winA
    winB.set_z_order(1);
    winB.fill_rect(0, 0, 200, 150, 0x2222); // Fill with color 0x2222

    // 4. Trigger Composition
    compositor.composite();

    // 5. Verify Screen Surface Output
    uint16_t* screen_buf = static_cast<uint16_t*>(screen.get_buffer());
    
    // Check outside any window (should be black 0x0000)
    EXPECT_EQ(screen_buf[50 * 800 + 50], 0x0000);
    
    // Check inside Window A, but outside Window B
    EXPECT_EQ(screen_buf[150 * 800 + 150], 0x1111);
    
    // Check inside Window B (overlapping area, B is on top so should be 0x2222)
    EXPECT_EQ(screen_buf[250 * 800 + 250], 0x2222);
}

TEST(GuixCompositorTest, DamageTracking) {
    SoftGpuDevice gpu;
    Surface screen(800, 600);
    Compositor compositor(&screen, &gpu);

    Window win(100, 100, &gpu, &compositor);
    win.move(0, 0);
    win.fill_rect(0, 0, 100, 100, 0xFFFF);

    // First composition should render the window
    compositor.composite();
    
    uint16_t* screen_buf = static_cast<uint16_t*>(screen.get_buffer());
    EXPECT_EQ(screen_buf[50 * 800 + 50], 0xFFFF);
    
    // Now, manually overwrite a pixel on the screen surface to a wrong color
    screen_buf[50 * 800 + 50] = 0x0000;
    
    // Call composite again without any damage
    compositor.composite();
    
    // Since there was no damage, the pixel should remain wrong (0x0000)
    EXPECT_EQ(screen_buf[50 * 800 + 50], 0x0000);
    
    // Now invalidate the window
    win.invalidate();
    compositor.composite();
    
    // The pixel should be fixed
    EXPECT_EQ(screen_buf[50 * 800 + 50], 0xFFFF);
}
