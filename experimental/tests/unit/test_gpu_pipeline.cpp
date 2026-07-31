#include <gtest/gtest.h>
#include "../../drivers/gpu/soft_gpu_device.hpp"
#include "../../drivers/gpu/surface.hpp"

using namespace auroraos::gpu;

TEST(GpuPipelineTest, FillRectCommand) {
    Surface surf(100, 100);
    SoftGpuDevice gpu;

    // Fill white (0xFFFF)
    GpuCommand cmd;
    cmd.opcode = GpuOpcode::FillRect;
    cmd.dst_surface = &surf;
    cmd.dst_x = 10;
    cmd.dst_y = 10;
    cmd.width = 20;
    cmd.height = 20;
    cmd.args.fill.color = 0xFFFF;

    gpu.submit(&cmd, 1);

    uint16_t* buf = static_cast<uint16_t*>(surf.get_buffer());
    // Inside the rect
    EXPECT_EQ(buf[15 * 100 + 15], 0xFFFF);
    // Outside the rect (default garbage or 0, depending on allocator, but since we didn't clear, we just check relative difference or assume we can't test outside strictly if not 0 initialized. Wait, new[] doesn't zero-initialize, let's zero it first).
    
    // Clear surface first
    GpuCommand clear_cmd;
    clear_cmd.opcode = GpuOpcode::FillRect;
    clear_cmd.dst_surface = &surf;
    clear_cmd.dst_x = 0; clear_cmd.dst_y = 0;
    clear_cmd.width = 100; clear_cmd.height = 100;
    clear_cmd.args.fill.color = 0x0000;
    gpu.submit(&clear_cmd, 1);
    
    // Fill white
    gpu.submit(&cmd, 1);
    
    EXPECT_EQ(buf[15 * 100 + 15], 0xFFFF); // Inside
    EXPECT_EQ(buf[5 * 100 + 5], 0x0000);   // Outside
}

TEST(GpuPipelineTest, BlitCommand) {
    Surface src(50, 50);
    Surface dst(100, 100);
    SoftGpuDevice gpu;
    
    // Clear dst
    GpuCommand clear_dst;
    clear_dst.opcode = GpuOpcode::FillRect;
    clear_dst.dst_surface = &dst;
    clear_dst.dst_x = 0; clear_dst.dst_y = 0;
    clear_dst.width = 100; clear_dst.height = 100;
    clear_dst.args.fill.color = 0x0000;
    gpu.submit(&clear_dst, 1);
    
    // Fill src with RED (0xF800)
    GpuCommand fill_src;
    fill_src.opcode = GpuOpcode::FillRect;
    fill_src.dst_surface = &src;
    fill_src.dst_x = 0; fill_src.dst_y = 0;
    fill_src.width = 50; fill_src.height = 50;
    fill_src.args.fill.color = 0xF800;
    gpu.submit(&fill_src, 1);
    
    // Blit src to dst at (10, 10)
    GpuCommand blit_cmd;
    blit_cmd.opcode = GpuOpcode::Blit;
    blit_cmd.dst_surface = &dst;
    blit_cmd.dst_x = 10;
    blit_cmd.dst_y = 10;
    blit_cmd.width = 30;
    blit_cmd.height = 30;
    blit_cmd.args.blit.src_surface = &src;
    blit_cmd.args.blit.src_x = 0;
    blit_cmd.args.blit.src_y = 0;
    
    gpu.submit(&blit_cmd, 1);
    
    uint16_t* dst_buf = static_cast<uint16_t*>(dst.get_buffer());
    EXPECT_EQ(dst_buf[20 * 100 + 20], 0xF800); // Inside blit
    EXPECT_EQ(dst_buf[5 * 100 + 5], 0x0000);   // Outside blit
}
