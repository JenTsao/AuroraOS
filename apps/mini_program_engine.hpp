#ifndef AURORA_MINI_PROGRAM_ENGINE_HPP
#define AURORA_MINI_PROGRAM_ENGINE_HPP

#include <stdint.h>
#include "posix.hpp"
#include "../drivers/sensor/sensor_framework.hpp"
#include "../drivers/display/framebuffer.hpp"
#include "../kernel/mm/memory.hpp"
#include "board.h"

// 引入第三方 Lua 虚拟机 C 接口
extern "C" {
#include "../3rdparty/lua/lua.h"
#include "../3rdparty/lua/lualib.h"
#include "../3rdparty/lua/lauxlib.h"
}
#include "lua_ui_binding.hpp"
#include "../ui/ui_config.hpp"
#include "../runtime/lua_heap.hpp"

// 声明外部全局的图形缓冲引擎 (用于供 Lua 脚本调用画图)
// 在 miband8 上使用条带化 framebuffer (AURORA_FB_CHUNK_HEIGHT = 30 行) 节省 SRAM
extern FrameBuffer<DISPLAY_WIDTH, AURORA_FB_CHUNK_HEIGHT> g_fb;

class MiniProgramEngine {
private:
    lua_State* L_;
    bool is_loaded_;
    LuaHeap<32768> lua_heap_; // 32KB 专属隔离堆：彻底消除 Lua GC 抖动对内核堆的碎片化污染

    // ========================================================
    // 指令预算（Instruction Budget）
    //
    // RTOS 宿主任务不可被不可信脚本饿死。通过 LUA_MASKCOUNT 计数钩子
    // 以 kInstructionQuantum 条指令为粒度扣减预算；预算耗尽即以 Lua 错误
    // 硬性终止当前脚本执行（经 pcall 捕获，宿主可控）。
    //
    // 语义：
    //  - 预算按"每次脚本入口"独立重置（load_app / load_app_from_file /
    //    call_hook 各自获得全新预算），不存在跨调用累计
    //  - instruction_budget_ == 0 表示不限制（仅限受信系统脚本使用）
    //  - 默认 50 万条 ≈ Cortex-M4 @80MHz 下几十毫秒量级，可按场景调整
    // ========================================================
    static constexpr uint32_t kDefaultInstructionBudget = 500000u;
    static constexpr uint32_t kInstructionQuantum = 1024u; // 钩子触发粒度（条）

    uint32_t instruction_budget_{kDefaultInstructionBudget};
    uint32_t budget_remaining_{0};

    // ========================================================
    // 钩子错误熔断器（Circuit Breaker）
    //
    // 防御劣质/恶意脚本以每帧抛错的方式刷爆 UART 或拖垮调度：
    // 连续 kMaxConsecutiveHookFailures 次 pcall 执行失败后，
    // 脚本被标记为 faulted，后续所有钩子调用快速失败（不再进入 VM）。
    // 宿主通过 reset_script_fault() 显式恢复（通常伴随脚本重载）。
    // 指令预算耗尽同样计入连续失败。
    // ========================================================
    static constexpr uint32_t kMaxConsecutiveHookFailures = 3u;

    uint32_t consecutive_hook_failures_{0};
    bool script_faulted_{false};

    // 一次性 UART 输出（open 失败时静默丢弃，不产生部分写入）
    static void uart_log(const char* msg) {
        const int fd = open("/dev/uart0", 0);
        if (fd >= 0) {
            int len = 0;
            while (msg[len])
                len++;
            write(fd, msg, len);
            close(fd);
        }
    }

    // 计数钩子：每执行 kInstructionQuantum 条 VM 指令触发一次。
    // 通过 lua_getallocf 取回引擎实例（lua_newstate 的 ud），无需额外 userdata。
    static void lua_instruction_hook(lua_State* L, lua_Debug* /*ar*/) {
        void* ud = nullptr;
        lua_getallocf(L, &ud);
        auto* self = static_cast<MiniProgramEngine*>(ud);
        if (!self) {
            lua_sethook(L, nullptr, 0, 0);
            return;
        }

        if (self->budget_remaining_ <= kInstructionQuantum) {
            // 先摘除钩子再抛错，避免错误对象构造期间的指令再次触发钩子
            lua_sethook(L, nullptr, 0, 0);
            luaL_error(L, "instruction budget exceeded");
            return;
        }
        self->budget_remaining_ -= kInstructionQuantum;
    }

    // 每个脚本入口处调用：重置预算并安装计数钩子
    void begin_execution_budget() {
        if (!L_)
            return;
        budget_remaining_ = instruction_budget_;
        if (instruction_budget_ == 0) {
            lua_sethook(L_, nullptr, 0, 0); // 不限制
        } else {
            lua_sethook(L_, lua_instruction_hook, LUA_MASKCOUNT,
                        static_cast<int>(kInstructionQuantum));
        }
    }

    // ========================================================
    // 自定义 Lua 内存分配器，将内存请求路由到专属私有池 LuaHeap
    // ========================================================
    static void* lua_allocator(void* ud, void* ptr, size_t osize, size_t nsize) {
        auto* self = static_cast<MiniProgramEngine*>(ud);
        if (!self) {
            return nullptr;
        }
        return self->lua_heap_.reallocate(ptr, osize, nsize);
    }

    // ========================================================
    // 系统 API 绑定 1：暴露获取心率的 C++ 函数给 Lua
    // ========================================================
    static int api_get_heart_rate(lua_State* L) {
        extern HeartRateSensor g_health_sensor;
        SensorDriver* hr_sensor = &g_health_sensor;
        if (hr_sensor) {
            SensorData val = {}; // Zero-initialize to prevent stack leaks
            if (hr_sensor->read(&val)) {
                lua_pushinteger(L, val.payload.bpm); // 将心率值压入 Lua 栈返回
            } else {
                lua_pushinteger(L, 0); // 传感器未上电或读取失败
            }
            return 1;
        }
        lua_pushinteger(L, 0);
        return 1;
    }

    // ========================================================
    // 系统 API 绑定 2：暴露 UI 绘制函数给 Lua
    // ========================================================
    static int api_fill_rect(lua_State* L) {
        // 从 Lua 获取 5 个参数: x, y, width, height, color_rgb565
        int x = luaL_checkinteger(L, 1);
        int y = luaL_checkinteger(L, 2);
        int w = luaL_checkinteger(L, 3);
        int h = luaL_checkinteger(L, 4);
        int color = luaL_checkinteger(L, 5);

        g_fb.fill_rect(x, y, w, h, static_cast<ColorRGB565>(color));
        return 0; // 无返回值
    }

    static int api_print(lua_State* L) {
        const char* str = luaL_checkstring(L, 1);
        int len = 0;
        while (str[len])
            len++;

        const int fd = open("/dev/uart0", 0);
        if (fd >= 0) {
            write(fd, "[Lua App] ", 10);
            write(fd, str, len);
            write(fd, "\r\n", 2);
            close(fd);
        }
        return 0;
    }

    // ========================================================
    // 系统 API 绑定 3：暴露获取时间的 C++ 函数给 Lua
    // ========================================================
    static int api_get_time(lua_State* L) {
        extern void aurora_get_time(uint32_t& h, uint32_t& m);
        uint32_t h = 0, m = 0;
        aurora_get_time(h, m);
        lua_pushinteger(L, h);
        lua_pushinteger(L, m);
        return 2;
    }

public:
    MiniProgramEngine() : L_(nullptr), is_loaded_(false) {}

    ~MiniProgramEngine() {
        if (L_) {
            lua_close(L_);
            L_ = nullptr;
        }
        lua_heap_.reset();
    }

    lua_State* get_lua_state() {
        return L_;
    }

    size_t get_used_memory() const noexcept {
        return lua_heap_.get_used_memory();
    }

    size_t get_free_memory() const noexcept {
        return lua_heap_.get_free_memory();
    }

    size_t get_total_memory() const noexcept {
        return lua_heap_.get_total_memory();
    }

    size_t get_peak_memory() const noexcept {
        return lua_heap_.get_peak_memory();
    }

    // ========================================================
    // 指令预算配置。budget == 0 表示不限制（仅限受信脚本）。
    // 必须在 init() 之后、脚本执行前设置。
    // ========================================================
    void set_instruction_budget(uint32_t max_instructions) noexcept {
        instruction_budget_ = max_instructions;
    }

    [[nodiscard]] uint32_t get_instruction_budget() const noexcept {
        return instruction_budget_;
    }

    // ========================================================
    // 熔断器状态与恢复。faulted 期间所有钩子/加载调用直接拒绝。
    // ========================================================
    [[nodiscard]] bool is_script_faulted() const noexcept {
        return script_faulted_;
    }

    // 宿主显式清除熔断（通常在重新加载修复后的脚本时调用）
    void reset_script_fault() noexcept {
        script_faulted_ = false;
        consecutive_hook_failures_ = 0;
    }

    // 初始化虚拟机并注册 API 命名空间
    bool init() {
        lua_heap_.reset();
        L_ = lua_newstate(lua_allocator, this); // 创建隔离的沙盒虚拟机，绑定专属 32KB Lua 私有池
        if (!L_)
            return false;

        // 我们裁剪了标准的 linit.c，只手动加载最核心的 base 库
        luaL_requiref(L_, "_G", luaopen_base, 1);
        lua_pop(L_, 1);

        // 彻底封堵预编译字节码沙盒逃逸漏洞：移除暴露的加载器
        lua_pushnil(L_);
        lua_setglobal(L_, "load");
        lua_pushnil(L_);
        lua_setglobal(L_, "loadstring");
        lua_pushnil(L_);
        lua_setglobal(L_, "dofile");
        lua_pushnil(L_);
        lua_setglobal(L_, "loadfile");

        // 将底层的 C++ 函数注册为 Lua 全局命名空间 aurora 的方法
        lua_newtable(L_);

        lua_pushcfunction(L_, api_get_heart_rate);
        lua_setfield(L_, -2, "get_heart_rate");

        lua_pushcfunction(L_, api_fill_rect);
        lua_setfield(L_, -2, "fill_rect");

        lua_pushcfunction(L_, api_print);
        lua_setfield(L_, -2, "print");

        lua_pushcfunction(L_, api_get_time);
        lua_setfield(L_, -2, "get_time");

        lua_setglobal(L_, "aurora"); // 注册全局变量 aurora

        luaopen_aurora_ui(L_); // 注册额外的 UI 控件

#ifdef CONFIG_NETWORKING
        // Register cybersecurity scanner bindings
        extern void register_scan_lua_bindings(lua_State * L);
        register_scan_lua_bindings(L_);

        // Register wireless IDS bindings
        extern void register_wireless_lua_bindings(lua_State * L);
        register_wireless_lua_bindings(L_);
#endif

        return true;
    }

    // 从字符串 (或 LittleFS 文件) 加载应用脚本代码
    bool load_app(const char* script_code) {
        if (!L_ || script_faulted_)
            return false;
        begin_execution_budget();
        if (luaL_dostring(L_, script_code) != LUA_OK) {
            uart_log("Lua Load Error!\r\n");
            return false;
        }
        is_loaded_ = true;
        return true;
    }

    // 从文件加载应用脚本代码
    bool load_app_from_file(const char* filepath) {
        if (!L_ || script_faulted_)
            return false;
        int fd = open(filepath, O_RDONLY);
        if (fd < 0)
            return false;

        // 0 = SEEK_SET, 2 = SEEK_END
        int size = lseek(fd, 0, 2);
        lseek(fd, 0, 0);

        if (size <= 0 || size > 65536) { // Sanity check max script size 64KB
            close(fd);
            return false;
        }

        char* buf = static_cast<char*>(KernelHeap::instance().allocate(size));
        if (!buf) {
            close(fd);
            return false;
        }

        int bytes_read = read(fd, buf, size);
        close(fd);

        if (bytes_read != size) {
            KernelHeap::instance().deallocate(buf);
            return false;
        }

        bool result = (luaL_loadbuffer(L_, buf, size, filepath) == LUA_OK);
        if (result) {
            begin_execution_budget();
            result = (lua_pcall(L_, 0, LUA_MULTRET, 0) == LUA_OK);
        }

        KernelHeap::instance().deallocate(buf);

        if (!result) {
            uart_log("Lua File Load Error!\r\n");
            return false;
        }

        is_loaded_ = true;
        return true;
    }

    // 触发脚本的生命周期钩子函数。
    // 返回 true = 钩子存在且执行成功；false = 钩子不存在、执行失败
    //（含指令预算耗尽）或脚本已熔断。所有历史调用方均忽略返回值，保持兼容。
    // 注意：钩子"未定义"属正常情况（脚本不必实现全部钩子），不计入熔断。
    bool call_hook(const char* hook_name) {
        if (!is_loaded_ || !L_)
            return false;
        if (script_faulted_) {
            return false; // 已熔断：快速失败，不再进入 VM 执行
        }

        lua_getglobal(L_, hook_name);
        if (lua_isfunction(L_, -1)) {
            begin_execution_budget();
            // 安全调用 Lua 函数，0个参数，0个返回值
            if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
                lua_pop(L_, 1); // 弹出错误信息

                // 熔断计数：连续失败达到阈值则禁用脚本执行（只告警一次）
                if (++consecutive_hook_failures_ >= kMaxConsecutiveHookFailures && !script_faulted_) {
                    script_faulted_ = true;
                    uart_log("[Lua] script faulted: too many consecutive hook errors, execution disabled\r\n");
                }
                return false;
            }
            consecutive_hook_failures_ = 0; // 成功执行重置连续失败计数
            return true;
        }
        lua_pop(L_, 1); // 如果不是函数则出栈
        return false;
    }
};

#endif
