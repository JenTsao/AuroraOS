#include "frame_scheduler_v2.hpp"

extern "C" bool frame_scheduler_is_task_allowed(uint8_t priority) {
    return FrameSchedulerV2::instance().is_task_allowed(priority);
}
