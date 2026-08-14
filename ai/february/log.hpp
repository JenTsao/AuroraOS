/**
 * @file log.hpp
 * @brief Optional debug logging for February Phase 1
 *
 * Enable: -DFEBRUARY_LOG_LEVEL=1 or 2. Default 0 = silent.
 */
#ifndef AURORA_FEBRUARY_LOG_HPP
#define AURORA_FEBRUARY_LOG_HPP

#ifndef FEBRUARY_LOG_LEVEL
#define FEBRUARY_LOG_LEVEL 0
#endif

namespace aurora {
namespace february {

using FebruaryLogFn = void (*)(const char* msg, void* user);

struct FebruaryLog {
    static FebruaryLogFn& sink() {
        static FebruaryLogFn fn = nullptr;
        return fn;
    }
    static void*& user() {
        static void* u = nullptr;
        return u;
    }
    static void set_sink(FebruaryLogFn fn, void* u = nullptr) {
        sink() = fn;
        user() = u;
    }
    static void write(const char* msg) {
        if (sink() && msg) sink()(msg, user());
    }
};

}  // namespace february
}  // namespace aurora

#if FEBRUARY_LOG_LEVEL >= 1
#define FEBRUARY_LOG(msg) ::aurora::february::FebruaryLog::write(msg)
#else
#define FEBRUARY_LOG(msg) ((void)0)
#endif

#if FEBRUARY_LOG_LEVEL >= 2
#define FEBRUARY_LOGV(msg) ::aurora::february::FebruaryLog::write(msg)
#else
#define FEBRUARY_LOGV(msg) ((void)0)
#endif

#endif
