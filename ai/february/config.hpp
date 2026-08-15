/**
 * @file config.hpp
 * @brief Compile-time feature flags for February (Kconfig-style)
 *
 * Map to real Kconfig (see ai/february/Kconfig):
 *   CONFIG_FEBRUARY=y
 *   CONFIG_FEBRUARY_SERVICE=y
 *   CONFIG_FEBRUARY_PLANNER=y
 *   CONFIG_FEBRUARY_SOFTBUS=y
 *   CONFIG_FEBRUARY_PEER_TABLE=y
 *
 * Defaults enable Phase 2.2 on host; strip on tiny MCUs via -D or Kconfig.
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

/** Fixed peer state table (last-seen / open / fail counts). */
#ifndef FEBRUARY_ENABLE_PEER_TABLE
#define FEBRUARY_ENABLE_PEER_TABLE 1
#endif

/**
 * When 1, remote SoftBus intents yield to pending local IntentDetected
 * on the same run_once (local wins). Remote still injects after local drain.
 */
#ifndef FEBRUARY_REMOTE_YIELD_TO_LOCAL
#define FEBRUARY_REMOTE_YIELD_TO_LOCAL 1
#endif

/** Max steps in one planned action sequence (zero-heap fixed array). */
#ifndef FEBRUARY_PLANNER_MAX_STEPS
#define FEBRUARY_PLANNER_MAX_STEPS 4
#endif

/** SoftBus local ring depth for remote / peer intents. */
#ifndef FEBRUARY_SOFTBUS_QUEUE_DEPTH
#define FEBRUARY_SOFTBUS_QUEUE_DEPTH 8
#endif

/** Max concurrent SoftBus sessions / registered peers. */
#ifndef FEBRUARY_SOFTBUS_MAX_SESSIONS
#define FEBRUARY_SOFTBUS_MAX_SESSIONS 4
#endif

/** Peer table slots (may equal sessions; can be larger for history). */
#ifndef FEBRUARY_PEER_TABLE_SIZE
#define FEBRUARY_PEER_TABLE_SIZE 4
#endif

/** Service loop default max events drained per process_events. */
#ifndef FEBRUARY_SERVICE_MAX_EVENTS
#define FEBRUARY_SERVICE_MAX_EVENTS 8
#endif

/** SoftBus package / session name defaults (overridable). */
#ifndef FEBRUARY_SOFTBUS_PKG
#define FEBRUARY_SOFTBUS_PKG "aurora.february"
#endif

#ifndef FEBRUARY_SOFTBUS_SESSION
#define FEBRUARY_SOFTBUS_SESSION "february.intent"
#endif

#endif  // AURORA_FEBRUARY_CONFIG_HPP
