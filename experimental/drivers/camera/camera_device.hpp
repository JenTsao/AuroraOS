#ifndef AURORA_CAMERA_DEVICE_HPP
#define AURORA_CAMERA_DEVICE_HPP

#include <stdint.h>
#include "../gpu/surface.hpp"

namespace auroraos {
namespace camera {

enum class PixelFormat : uint8_t {
    RGB565 = 0,
    YUV422 = 1,
    JPEG = 2
};

// Callback type for when a frame is captured. 
// Provides the Surface containing the frame.
using FrameCallback = void (*)(gpu::Surface* frame, void* user_data);

class CameraDevice {
public:
    virtual ~CameraDevice() = default;

    // Initialize the camera sensor with target resolution and format
    virtual bool init(uint16_t width, uint16_t height, PixelFormat fmt) = 0;
    
    // Register the callback to be invoked on frame arrival
    virtual void set_frame_callback(FrameCallback callback, void* user_data) = 0;
    
    // Start capturing stream
    virtual void start_capture() = 0;
    
    // Stop capturing
    virtual void stop_capture() = 0;
};

} // namespace camera
} // namespace auroraos

#endif // AURORA_CAMERA_DEVICE_HPP
