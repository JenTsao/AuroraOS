#include <gtest/gtest.h>
#include "../../guix/window.hpp"
#include "../../guix/compositor.hpp"
#include "../../guix/guix.hpp"
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

TEST(GuixCompositorTest, WindowVisibilityToggle) {
    SoftGpuDevice gpu;
    Surface screen(400, 300);
    Compositor compositor(&screen, &gpu);
    compositor.set_background_color(Color::Black);

    Window win(100, 100, &gpu, &compositor);
    win.move(50, 50);
    win.fill_rect(0, 0, 100, 100, Color::Red);

    compositor.composite();
    uint16_t* screen_buf = static_cast<uint16_t*>(screen.get_buffer());
    EXPECT_EQ(screen_buf[60 * 400 + 60], Color::Red);

    // Hide window -> next composition should clear to background
    win.set_visible(false);
    EXPECT_FALSE(win.is_visible());
    compositor.composite();
    EXPECT_EQ(screen_buf[60 * 400 + 60], Color::Black);

    // Show window again
    win.set_visible(true);
    EXPECT_TRUE(win.is_visible());
    compositor.composite();
    EXPECT_EQ(screen_buf[60 * 400 + 60], Color::Red);
}

TEST(GuixCompositorTest, WindowZOrderingAndBringToFront) {
    SoftGpuDevice gpu;
    Surface screen(400, 300);
    Compositor compositor(&screen, &gpu);

    Window win1(100, 100, &gpu, &compositor);
    win1.set_id(1);
    win1.move(50, 50);
    win1.set_z_order(10);
    win1.fill_rect(0, 0, 100, 100, Color::Blue);

    Window win2(100, 100, &gpu, &compositor);
    win2.set_id(2);
    win2.move(80, 80);
    win2.set_z_order(20);
    win2.fill_rect(0, 0, 100, 100, Color::Green);

    compositor.composite();
    uint16_t* screen_buf = static_cast<uint16_t*>(screen.get_buffer());

    // Overlapping region (90, 90): win2 is on top
    EXPECT_EQ(screen_buf[90 * 400 + 90], Color::Green);

    // Bring win1 to front
    win1.bring_to_front();
    EXPECT_GT(win1.get_z_order(), win2.get_z_order());

    compositor.composite();
    EXPECT_EQ(screen_buf[90 * 400 + 90], Color::Blue);

    // Send win1 to back
    win1.send_to_back();
    EXPECT_LT(win1.get_z_order(), win2.get_z_order());

    compositor.composite();
    EXPECT_EQ(screen_buf[90 * 400 + 90], Color::Green);
}

TEST(GuixCompositorTest, HitTestingAndPointerRouting) {
    SoftGpuDevice gpu;
    Surface screen(400, 300);
    Compositor compositor(&screen, &gpu);

    Window winA(100, 100, &gpu, &compositor);
    winA.set_id(101);
    winA.move(10, 10);
    winA.set_z_order(1);

    Window winB(100, 100, &gpu, &compositor);
    winB.set_id(102);
    winB.move(50, 50);
    winB.set_z_order(2); // On top of winA in overlapping region

    // Hit outside both
    EXPECT_EQ(compositor.find_window_at(5, 5), nullptr);

    // Hit inside winA only
    EXPECT_EQ(compositor.find_window_at(20, 20), &winA);

    // Hit in overlapping region (60, 60): should return top-most winB
    EXPECT_EQ(compositor.find_window_at(60, 60), &winB);

    // Track input events
    static int received_id = 0;
    static int local_x = -1, local_y = -1;

    auto handler = [](Window* win, const InputEvent& ev, void*) {
        if (ev.type == InputEventType::PointerDown) {
            received_id = win->get_id();
            local_x = ev.x;
            local_y = ev.y;
        }
    };

    winA.set_event_handler(handler);
    winB.set_event_handler(handler);

    InputEvent click{InputEventType::PointerDown, 70, 80, 0, 1000, 0};
    EXPECT_TRUE(compositor.dispatch_input_event(click));
    EXPECT_EQ(received_id, 102); // Routed to winB
    EXPECT_EQ(local_x, 70 - 50);  // Local x = 20
    EXPECT_EQ(local_y, 80 - 50);  // Local y = 30
    EXPECT_EQ(compositor.get_focused_window(), &winB);
}

TEST(GuixCompositorTest, WindowResizePreservesContent) {
    SoftGpuDevice gpu;
    Surface screen(400, 300);
    Compositor compositor(&screen, &gpu);

    Window win(50, 50, &gpu, &compositor);
    win.fill_rect(0, 0, 50, 50, Color::Yellow);

    EXPECT_EQ(win.get_width(), 50u);
    EXPECT_EQ(win.get_height(), 50u);

    // Resize window to 100x100
    EXPECT_TRUE(win.resize(100, 100));
    EXPECT_EQ(win.get_width(), 100u);
    EXPECT_EQ(win.get_height(), 100u);

    // Old area (25, 25) should preserve Yellow
    uint16_t* win_buf = static_cast<uint16_t*>(win.get_surface()->get_buffer());
    EXPECT_EQ(win_buf[25 * 100 + 25], Color::Yellow);
    // Newly expanded area (75, 75) should be initialized to 0
    EXPECT_EQ(win_buf[75 * 100 + 75], 0x0000);
}

TEST(GuixCompositorTest, SubRectDamageAndDrawingPrimitives) {
    SoftGpuDevice gpu;
    Surface screen(400, 300);
    Compositor compositor(&screen, &gpu);

    Window win(200, 200, &gpu, &compositor);
    win.move(10, 10);
    win.clear(Color::Black);

    // Test Drawing Primitives on Window
    win.draw_pixel(5, 5, Color::White);
    win.draw_line(10, 10, 30, 10, Color::Red);
    win.draw_rect(40, 40, 20, 20, Color::Green);
    win.draw_circle(100, 100, 15, Color::Cyan);
    win.fill_circle(150, 150, 10, Color::Magenta);
    win.draw_text(10, 170, "GUIX", Color::White, Color::Black, false, 1);

    compositor.composite();
    uint16_t* screen_buf = static_cast<uint16_t*>(screen.get_buffer());

    // Check pixel at screen coord (10 + 5, 10 + 5) = (15, 15)
    EXPECT_EQ(screen_buf[15 * 400 + 15], Color::White);
    // Check line at screen coord (10 + 20, 10 + 10) = (30, 20)
    EXPECT_EQ(screen_buf[20 * 400 + 30], Color::Red);
    // Check filled circle center at (10 + 150, 10 + 150) = (160, 160)
    EXPECT_EQ(screen_buf[160 * 400 + 160], Color::Magenta);
}

TEST(GuixCompositorTest, BackgroundWallpaperBlit) {
    SoftGpuDevice gpu;
    Surface screen(200, 200);
    Compositor compositor(&screen, &gpu);

    // Create a background wallpaper surface filled with Cyan
    Surface wallpaper(200, 200);
    uint16_t* wp_buf = static_cast<uint16_t*>(wallpaper.get_buffer());
    for (int i = 0; i < 200 * 200; ++i) {
        wp_buf[i] = Color::Cyan;
    }
    compositor.set_background_surface(&wallpaper);

    // Create a small window filled with Red in the center
    Window win(50, 50, &gpu, &compositor);
    win.move(75, 75);
    win.fill_rect(0, 0, 50, 50, Color::Red);

    compositor.composite();
    uint16_t* screen_buf = static_cast<uint16_t*>(screen.get_buffer());

    // Outside window: Wallpaper Cyan
    EXPECT_EQ(screen_buf[10 * 200 + 10], Color::Cyan);
    // Inside window: Window Red
    EXPECT_EQ(screen_buf[100 * 200 + 100], Color::Red);
}

TEST(GuixCompositorTest, AlphaTransparencyBlending) {
    SoftGpuDevice gpu;
    Surface screen(200, 200);
    Compositor compositor(&screen, &gpu);
    compositor.set_background_color(Color::White); // Screen is white

    // Create window with 50% opacity (alpha = 128) and filled with Black
    Window win(100, 100, &gpu, &compositor);
    win.move(50, 50);
    win.set_alpha(128);
    win.clear(Color::Black);

    compositor.composite();
    uint16_t* screen_buf = static_cast<uint16_t*>(screen.get_buffer());

    // Blended pixel inside window (50% black + 50% white -> gray)
    uint16_t pixel = screen_buf[75 * 200 + 75];
    EXPECT_NE(pixel, Color::Black);
    EXPECT_NE(pixel, Color::White);
}
