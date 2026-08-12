#ifndef AURORAOS_RUNTIME_APP_BASE_HPP
#define AURORAOS_RUNTIME_APP_BASE_HPP

#include "app_state.hpp"

namespace auroraos {
namespace runtime {

class AppBase {
public:
    AppBase(const char* name) : name_(name), state_(AppState::Created) {}
    virtual ~AppBase() = default;

    const char* get_name() const { return name_; }
    AppState get_state() const { return state_; }

    // Lifecycle methods called by Aurora Runtime
    bool start();
    void pause();
    void resume();
    void stop();
    void destroy();

protected:
    // Virtual callbacks for subclasses to implement
    virtual bool on_start() { return true; }
    virtual void on_pause() {}
    virtual void on_resume() {}
    virtual void on_stop() {}
    virtual void on_destroy() {}
    virtual void on_error(const char* reason) {}

    // Method to trigger state change to Error internally
    void fault(const char* reason);

private:
    const char* name_;
    AppState state_;
};

} // namespace runtime
} // namespace auroraos

#endif // AURORAOS_RUNTIME_APP_BASE_HPP
