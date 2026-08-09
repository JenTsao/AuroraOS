#include "../drivers/camera/dummy_camera.hpp"
#include "../guix/window.hpp"
#include "../guix/compositor.hpp"
#include "../drivers/gpu/gpu_device.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace auroraos {
namespace apps {

class CameraApp {
public:
    CameraApp(guix::Compositor* compositor, gpu::GpuDevice* gpu)
        : compositor_(compositor), gpu_(gpu) {

        // Create a 240x240 window for the camera viewfinder
        window_ = new guix::Window(240, 240, gpu_, compositor_);
        window_->move(50, 50);
        window_->set_z_order(10);

        // Initialize camera to match window resolution
        camera_.init(240, 240, camera::PixelFormat::RGB565);

        // Register static callback, passing 'this' as user_data
        camera_.set_frame_callback(on_frame_arrived_static, this);

        // Start streaming
        camera_.start_capture();

#ifdef DEBUG_CAMERA
        int fd = open("/dev/uart0", O_WRONLY);
        if (fd >= 0) {
            const char msg[] = "\r\n[CameraApp] Constructed. Window=240x240 @ (50,50), capture started.\r\n";
            write(fd, msg, sizeof(msg) - 1);
            close(fd);
        }
#endif
    }

    ~CameraApp() {
        camera_.stop_capture();
        delete window_;

#ifdef DEBUG_CAMERA
        int fd = open("/dev/uart0", O_WRONLY);
        if (fd >= 0) {
            const char msg[] = "\r\n[CameraApp] Destructed. Capture stopped, window destroyed.\r\n";
            write(fd, msg, sizeof(msg) - 1);
            close(fd);
        }
#endif
    }

    // Time-driven main loop tick.
    // The caller (scheduler / main loop) advances simulation by `delta_ms`.
    // We use it to:
    //   1) drive a dummy frame arrival on the camera,
    //   2) restart the capture stream every tick to keep the pipeline alive
    //      in case the upstream stopped due to error recovery or stream stall.
    void tick(uint32_t delta_ms) {
#ifdef DEBUG_CAMERA
        int fd_dbg = open("/dev/uart0", O_WRONLY);
        if (fd_dbg >= 0) {
            const char msg[] = "\r\n[CameraApp] tick(delta_ms) called.\r\n";
            write(fd_dbg, msg, sizeof(msg) - 1);
            close(fd_dbg);
        }
#endif

        // Drive the dummy camera: produces one frame and fires the registered callback.
        camera_.simulate_frame_arrival();

        // Stream restart: re-assert the capture state so transient stop/stall
        // conditions are self-healed on every scheduler tick.
        camera_.start_capture();

        (void)delta_ms; // 当前 dummy 驱动不消费 delta_ms，保留供真实驱动实现按时间步进
    }

private:
    static void on_frame_arrived_static(gpu::Surface* frame, void* user_data) {
        auto* app = static_cast<CameraApp*>(user_data);
        app->on_frame_arrived(frame);
    }

    void on_frame_arrived(gpu::Surface* frame) {
        // Defensive programming: split null/invalidity checks so each
        // guard is independent and produces a clear debug breadcrumb
        // (when DEBUG_CAMERA is enabled) without extra branch noise
        // in release builds.
        if (!frame) {
#ifdef DEBUG_CAMERA
            int fd_dbg = open("/dev/uart0", O_WRONLY);
            if (fd_dbg >= 0) {
                const char msg[] = "\r\n[CameraApp] on_frame_arrived: null frame, drop.\r\n";
                write(fd_dbg, msg, sizeof(msg) - 1);
                close(fd_dbg);
            }
#endif
            return;
        }
        if (!window_) {
#ifdef DEBUG_CAMERA
            int fd_dbg = open("/dev/uart0", O_WRONLY);
            if (fd_dbg >= 0) {
                const char msg[] = "\r\n[CameraApp] on_frame_arrived: null window, drop frame.\r\n";
                write(fd_dbg, msg, sizeof(msg) - 1);
                close(fd_dbg);
            }
#endif
            return;
        }

        // The camera hardware has DMA'd the new frame into 'frame' Surface.
        // We now use the GPU driver to Blit this frame onto our Window's backing store.
        gpu::GpuCommand blit;
        blit.opcode = gpu::GpuOpcode::Blit;
        blit.dst_surface = window_->get_surface();
        blit.dst_x = 0;
        blit.dst_y = 0;
        blit.width = 240;
        blit.height = 240;
        blit.args.blit.src_surface = frame;
        blit.args.blit.src_x = 0;
        blit.args.blit.src_y = 0;

        gpu_->submit(&blit, 1);

        // Notify the compositor that our window is dirty and needs to be re-composited to the screen
        window_->invalidate();

#ifdef DEBUG_CAMERA
        int fd_dbg = open("/dev/uart0", O_WRONLY);
        if (fd_dbg >= 0) {
            const char msg[] = "\r\n[CameraApp] Frame arrived, blit submitted, window invalidated.\r\n";
            write(fd_dbg, msg, sizeof(msg) - 1);
            close(fd_dbg);
        }
#endif
    }

    guix::Compositor* compositor_;
    gpu::GpuDevice* gpu_;
    guix::Window* window_;
    camera::DummyCamera camera_;
};

} // namespace apps
} // namespace auroraos
