/**
 * @file config.hpp
 * @brief Compile-time feature flags for February (Kconfig-style)
 *
 * Map these to real Kconfig symbols later, e.g.:
 *   CONFIG_FEBRUARY=y
 *   CONFIG_FEBRUARY_SERVICE=y
 *   CONFIG_FEBRUARY_PLANNER=y
 *   CONFIG_FEBRUARY_SOFTBUS=y
 *
 * Defaults enable the Phase 2 skeleton on host; strip on tiny MCUs via -D.
 */
#ifndef AURORA_FEBRUARY_CONFIG_HPP
#define AURORA_FEBRUARY_CONFIG_HPP

#ifndef FEBRUARY_ENABLE_SERVICE
#define FEBRUARY_ENABLE_SERVICE 1
#endif

#ifndef FEBRUARY_ENABLE_PLANNER
#define FEBRUARY_ENABLE_PLANNER 1
#endif

#ifndef FEBRUARY_ENABLE_SOFTBUS
#define FEBRUARY_ENABLE_SOFTBUS 1
#endif

/** Max steps in one planned action sequence (zero-heap fixed array). */
#ifndef FEBRUARY_PLANNER_MAX_STEPS
#define FEBRUARY_PLANNER_MAX_STEPS 4
#endif

/** SoftBus local ring depth for remote / peer intents. */
#ifndef FEBRUARY_SOFTBUS_QUEUE_DEPTH
#define FEBRUARY_SOFTBUS_QUEUE_DEPTH 8
#endif

/** Service loop default max events drained per process_events. */
#ifndef FEBRUARY_SERVICE_MAX_EVENTS
#define FEBRUARY_SERVICE_MAX_EVENTS 8
#endif

#endif  // AURORA_FEBRUARY_CONFIG_HPP
