#ifndef AURORA_JS_MINI_PROGRAM_ENGINE_HPP
#define AURORA_JS_MINI_PROGRAM_ENGINE_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "posix.hpp"
#include "../drivers/sensor/sensor_framework.hpp"
#include "../drivers/display/framebuffer.hpp"
#include "../kernel/mm/memory.hpp"
#include "board.h"
#include "../ui/ui_config.hpp"
#include "../runtime/js_heap.hpp"

extern "C" {
#include "../3rdparty/quickjs/quickjs.h"
}

// 声明外部全局图形缓冲引擎 (在 miband8/qemu 上用于供 JS 脚本画图)
extern FrameBuffer<DISPLAY_WIDTH, AURORA_UI_BAND_H> g_fb;

template <size_t HeapSize = 131072> class JsMiniProgramEngine {
private:
    JSRuntime* rt_{nullptr};
    JSContext* ctx_{nullptr};
    bool is_loaded_{false};
    JsHeap<HeapSize> js_heap_; // 专属隔离堆：彻底消除 QuickJS GC 与对象分配对内核堆的碎片化污染

    // ========================================================
    // 指令预算（Execution Budget）
    //
    // RTOS 宿主任务不可被死循环不可信脚本饿死。通过 QuickJS
    // JS_SetInterruptHandler 回调，以 kInstructionQuantum 为粒度
    // 扣减预算；预算耗尽时触发中断并中止脚本执行。
    // ========================================================
    static constexpr uint32_t kDefaultInstructionBudget = 500000u;
    static constexpr uint32_t kInstructionQuantum = 1024u;

    uint32_t instruction_budget_{kDefaultInstructionBudget};
    uint32_t budget_remaining_{0};

    // ========================================================
    // 钩子错误熔断器（Circuit Breaker）
    //
    // 防御劣质/恶意脚本以每帧抛错的方式刷爆 UART 或拖垮调度：
    // 连续 kMaxConsecutiveHookFailures 次执行失败后，
    // 脚本被标记为 faulted，后续所有钩子调用快速失败。
    // ========================================================
    static constexpr uint32_t kMaxConsecutiveHookFailures = 3u;

    uint32_t consecutive_hook_failures_{0};
    bool script_faulted_{false};

    static void uart_log(const char* msg) {
        if (!msg)
            return;
        const int fd = open("/dev/uart0", 0);
        if (fd >= 0) {
            int len = 0;
            while (msg[len])
                len++;
            write(fd, msg, len);
            write(fd, "\r\n", 2);
            close(fd);
        }
    }

    static int js_interrupt_handler(JSRuntime* /*rt*/, void* opaque) {
        auto* self = static_cast<JsMiniProgramEngine*>(opaque);
        if (!self)
            return 1;
        if (self->instruction_budget_ == 0)
            return 0; // 不限制

        if (self->budget_remaining_ <= kInstructionQuantum) {
            return 1; // 预算耗尽，触发 QuickJS 内部中断
        }
        self->budget_remaining_ -= kInstructionQuantum;
        return 0;
    }

    void begin_execution_budget() {
        budget_remaining_ = instruction_budget_;
    }

    void handle_execution_failure() {
        consecutive_hook_failures_++;
        if (consecutive_hook_failures_ >= kMaxConsecutiveHookFailures) {
            script_faulted_ = true;
            uart_log("[JS Engine] Circuit breaker tripped: script faulted due to consecutive failures");
        }
    }

    // ========================================================
    // Native API 绑定 1：UART 打印 (aurora.print & console.log)
    // ========================================================
    static JSValue api_print(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
        if (argc < 1)
            return JS_UNDEFINED;

        const char* str = JS_ToCString(ctx, argv[0]);
        if (!str)
            return JS_UNDEFINED;

        int len = 0;
        while (str[len])
            len++;

        const int fd = open("/dev/uart0", 0);
        if (fd >= 0) {
            write(fd, "[JS App] ", 9);
            write(fd, str, len);
            write(fd, "\r\n", 2);
            close(fd);
        }
        JS_FreeCString(ctx, str);
        return JS_UNDEFINED;
    }

    // ========================================================
    // Native API 绑定 2：读取心率 (aurora.getHeartRate)
    // ========================================================
    static JSValue api_get_heart_rate(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
        extern HeartRateSensor g_health_sensor;
        SensorDriver* hr_sensor = &g_health_sensor;
        if (hr_sensor) {
            SensorData val = {};
            if (hr_sensor->read(&val)) {
                return JS_NewInt32(ctx, val.payload.bpm);
            }
        }
        return JS_NewInt32(ctx, 0);
    }

    // ========================================================
    // Native API 绑定 3：绘制矩形 (aurora.fillRect)
    // ========================================================
    static JSValue api_fill_rect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
        if (argc < 5)
            return JS_UNDEFINED;

        int32_t x = 0, y = 0, w = 0, h = 0, color = 0;
        JS_ToInt32(ctx, &x, argv[0]);
        JS_ToInt32(ctx, &y, argv[1]);
        JS_ToInt32(ctx, &w, argv[2]);
        JS_ToInt32(ctx, &h, argv[3]);
        JS_ToInt32(ctx, &color, argv[4]);

        g_fb.fill_rect(x, y, w, h, static_cast<ColorRGB565>(color));
        return JS_UNDEFINED;
    }

    // ========================================================
    // Native API 绑定 4：获取系统时间 (aurora.getTime)
    // ========================================================
    static JSValue api_get_time(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
        extern void aurora_get_time(uint32_t& h, uint32_t& m);
        uint32_t h = 0, m = 0;
        aurora_get_time(h, m);

        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "hour", JS_NewInt32(ctx, static_cast<int32_t>(h)));
        JS_SetPropertyStr(ctx, obj, "minute", JS_NewInt32(ctx, static_cast<int32_t>(m)));
        return obj;
    }

    void register_aurora_api() {
        if (!ctx_)
            return;

        JSValue global_obj = JS_GetGlobalObject(ctx_);

        // 1. aurora 命名空间
        JSValue aurora_obj = JS_NewObject(ctx_);
        JS_SetPropertyStr(ctx_, aurora_obj, "print", JS_NewCFunction(ctx_, api_print, "print", 1));
        JS_SetPropertyStr(ctx_, aurora_obj, "getHeartRate",
                          JS_NewCFunction(ctx_, api_get_heart_rate, "getHeartRate", 0));
        JS_SetPropertyStr(ctx_, aurora_obj, "fillRect", JS_NewCFunction(ctx_, api_fill_rect, "fillRect", 5));
        JS_SetPropertyStr(ctx_, aurora_obj, "getTime", JS_NewCFunction(ctx_, api_get_time, "getTime", 0));

        JS_SetPropertyStr(ctx_, global_obj, "aurora", aurora_obj);

        // 2. 惯用 console 命名空间
        JSValue console_obj = JS_NewObject(ctx_);
        JS_SetPropertyStr(ctx_, console_obj, "log", JS_NewCFunction(ctx_, api_print, "log", 1));
        JS_SetPropertyStr(ctx_, console_obj, "warn", JS_NewCFunction(ctx_, api_print, "warn", 1));
        JS_SetPropertyStr(ctx_, console_obj, "error", JS_NewCFunction(ctx_, api_print, "error", 1));

        JS_SetPropertyStr(ctx_, global_obj, "console", console_obj);

        JS_FreeValue(ctx_, global_obj);
    }

public:
    JsMiniProgramEngine() = default;

    ~JsMiniProgramEngine() {
        teardown();
    }

    void teardown() {
        if (ctx_) {
            JS_FreeContext(ctx_);
            ctx_ = nullptr;
        }
        if (rt_) {
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
        }
        js_heap_.reset();
        is_loaded_ = false;
        script_faulted_ = false;
        consecutive_hook_failures_ = 0;
    }

    JSContext* get_context() const noexcept {
        return ctx_;
    }

    JSRuntime* get_runtime() const noexcept {
        return rt_;
    }

    [[nodiscard]] size_t get_used_memory() const noexcept {
        return js_heap_.get_used_memory();
    }

    [[nodiscard]] size_t get_free_memory() const noexcept {
        return js_heap_.get_free_memory();
    }

    [[nodiscard]] size_t get_total_memory() const noexcept {
        return js_heap_.get_total_memory();
    }

    [[nodiscard]] size_t get_peak_memory() const noexcept {
        return js_heap_.get_peak_memory();
    }

    void set_instruction_budget(uint32_t budget) noexcept {
        instruction_budget_ = budget;
    }

    [[nodiscard]] uint32_t get_instruction_budget() const noexcept {
        return instruction_budget_;
    }

    [[nodiscard]] bool is_script_faulted() const noexcept {
        return script_faulted_;
    }

    void reset_script_fault() noexcept {
        script_faulted_ = false;
        consecutive_hook_failures_ = 0;
    }

    bool init() {
        teardown();
        js_heap_.reset();

        JSMallocFunctions mf = JsHeap<HeapSize>::get_malloc_functions();
        rt_ = JS_NewRuntime2(&mf, &js_heap_);
        if (!rt_) {
            return false;
        }

        JS_SetMemoryLimit(rt_, HeapSize);
        JS_SetMaxStackSize(rt_, 32768);
        JS_SetInterruptHandler(rt_, js_interrupt_handler, this);

        ctx_ = JS_NewContext(rt_);
        if (!ctx_) {
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
            return false;
        }

        register_aurora_api();
        return true;
    }

    bool load_app(const char* code, size_t len = 0) {
        if (script_faulted_ || !ctx_) {
            return false;
        }
        if (len == 0 && code) {
            len = strlen(code);
        }

        begin_execution_budget();
        JSValue ret = JS_Eval(ctx_, code, len, "<app>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(ctx_);
            const char* msg = JS_ToCString(ctx_, ex);
            uart_log(msg ? msg : "JS load_app exception");
            JS_FreeCString(ctx_, msg);
            JS_FreeValue(ctx_, ex);
            JS_FreeValue(ctx_, ret);
            handle_execution_failure();
            return false;
        }

        JS_FreeValue(ctx_, ret);
        consecutive_hook_failures_ = 0;
        is_loaded_ = true;
        return true;
    }

    bool load_app_from_file(const char* path) {
        if (!path)
            return false;
        int fd = open(path, 0);
        if (fd < 0)
            return false;

        static constexpr size_t kMaxScriptSize = 16384;
        char* buf = static_cast<char*>(js_heap_.allocate(kMaxScriptSize + 1));
        if (!buf) {
            close(fd);
            return false;
        }

        int bytes_read = read(fd, buf, kMaxScriptSize);
        close(fd);

        if (bytes_read < 0) {
            js_heap_.deallocate(buf);
            return false;
        }

        buf[bytes_read] = '\0';
        bool ok = load_app(buf, static_cast<size_t>(bytes_read));
        js_heap_.deallocate(buf);
        return ok;
    }

    bool call_hook(const char* hook_name) {
        if (script_faulted_ || !ctx_ || !is_loaded_ || !hook_name) {
            return false;
        }

        JSValue global_obj = JS_GetGlobalObject(ctx_);
        JSValue hook_func = JS_GetPropertyStr(ctx_, global_obj, hook_name);
        JSValue this_target = global_obj;
        bool need_free_this = false;

        // 若顶级未声明，尝试在 global.app 或 global.AppInstance 对象上查找
        if (!JS_IsFunction(ctx_, hook_func)) {
            JS_FreeValue(ctx_, hook_func);
            JSValue app_obj = JS_GetPropertyStr(ctx_, global_obj, "app");
            if (JS_IsObject(app_obj)) {
                hook_func = JS_GetPropertyStr(ctx_, app_obj, hook_name);
                this_target = app_obj;
                need_free_this = true;
            } else {
                JS_FreeValue(ctx_, app_obj);
                JS_FreeValue(ctx_, global_obj);
                return true; // 钩子未定义为正常空操作，非错误
            }
        }

        bool ok = true;
        if (JS_IsFunction(ctx_, hook_func)) {
            begin_execution_budget();
            JSValue ret = JS_Call(ctx_, hook_func, this_target, 0, nullptr);
            if (JS_IsException(ret)) {
                JSValue ex = JS_GetException(ctx_);
                const char* msg = JS_ToCString(ctx_, ex);
                uart_log(msg ? msg : "JS hook exception");
                JS_FreeCString(ctx_, msg);
                JS_FreeValue(ctx_, ex);
                handle_execution_failure();
                ok = false;
            } else {
                consecutive_hook_failures_ = 0;
            }
            JS_FreeValue(ctx_, ret);
        }

        JS_FreeValue(ctx_, hook_func);
        if (need_free_this) {
            JS_FreeValue(ctx_, this_target);
        }
        JS_FreeValue(ctx_, global_obj);
        return ok;
    }
};

#endif // AURORA_JS_MINI_PROGRAM_ENGINE_HPP
