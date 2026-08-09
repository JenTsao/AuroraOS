#ifndef AURORA_DISPLAY_CONTROLLER_HPP
#define AURORA_DISPLAY_CONTROLLER_HPP

#include <stdint.h>
#include "surface.hpp"

namespace auroraos {
namespace gpu {

class DisplayController {
public:
    virtual ~DisplayController() = default;

    // Set the framebuffer surface to scan out to the physical display
    virtual void set_active_framebuffer(Surface* surface) = 0;

    // Wait until the next VSYNC (Vertical Blanking Interval) to avoid tearing
    virtual void wait_vsync() = 0;
};

} // namespace gpu
} // namespace auroraos

#endif // AURORA_DISPLAY_CONTROLLER_HPP
