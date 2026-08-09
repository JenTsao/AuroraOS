// Default no-op watchdog_feed when CONFIG_WATCHDOG is disabled.
// Weak symbol — overridden by the inline in watchdog_manager.hpp when enabled.
#include "task.hpp"

__attribute__((weak)) void watchdog_feed(uint32_t) {}
