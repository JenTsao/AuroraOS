#ifndef AURORA_DUMMY_CAMERA_HPP
#define AURORA_DUMMY_CAMERA_HPP

#include "camera_device.hpp"

namespace auroraos {
namespace camera {

class DummyCamera : public CameraDevice {
public:
    DummyCamera();
    ~DummyCamera() override;

    bool init(uint16_t width, uint16_t height, PixelFormat fmt) override;
    // ⚠️ 生命周期约定 (Lifecycle Contract):
    // 传递给 callback 的 Surface 指针生命周期由 Camera 驱动严格拥有。
    // 接收方在 callback 执行期间可同步读取数据，或立即发起深度拷贝。
    // 严禁在上层逻辑中长期保存该指针，因为在 init() 重置、分辨率变更或驱动析构时，
    // 该 Surface 会被销毁，导致保存的指针悬空。
    void set_frame_callback(FrameCallback callback, void* user_data) override;
    void start_capture() override;
    void stop_capture() override;

    // Simulate an interrupt or timer tick arriving
    void simulate_frame_arrival();

private:
    uint16_t width_;
    uint16_t height_;
    PixelFormat format_;
    bool capturing_;
    uint32_t frame_count_;
    
    gpu::Surface* frame_surface_;
    
    FrameCallback callback_;
    void* user_data_;
};

} // namespace camera
} // namespace auroraos

#endif // AURORA_DUMMY_CAMERA_HPP
