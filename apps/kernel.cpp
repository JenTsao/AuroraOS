#include "uart.h"
#include "config.h"
#include "net_config.h"
#include "interrupts.hpp"
#include "task.hpp"
#include "memory.hpp"
#include "vfs.hpp"
#include "device.hpp"
#include "ramfs.hpp"
#include "shell.hpp" // 引入 Shell
#include "syscall.hpp"
#include "mutex.hpp"
#include "../net/eth_driver.hpp"
#include "task_notify.hpp"
#include "signal.hpp"
#include "frame_scheduler_v2.hpp"
#ifdef CONFIG_NETWORKING
#include "../net/ble/ble_signature.hpp"
#endif
#include "../drivers/display/oled_driver.hpp"
#include "../drivers/display/framebuffer.hpp"
#include "../drivers/input/touch_driver.hpp" // 引入触控驱动
#include "../drivers/input/input_event.hpp"  // 引入输入协议
#include "../drivers/sensor/sensor_framework.hpp" // 传感器框架
#include "../ui/complications.hpp"    // 小组件引擎
#include "../kernel/app_lifecycle.hpp" // 应用生命周期管理
#ifdef CONFIG_NETWORKING
#include "../ai/intent_engine.hpp"     // AI 意图引擎
#endif
#include "../apps/mini_program_engine.hpp" // 小程序引擎
#include "../vfs/photon_cache.hpp"
#include "../vfs/littlefs_vnode.hpp"
extern Mutex uart_mutex;
#include "mpu.hpp"
#ifdef CONFIG_NETWORKING
#include "net_app.hpp"
#endif
#include "gesture_recognizer.hpp"
#include "font_engine.hpp"

// 包装一下入口函数以符合 create_task 的签名
#ifdef CONFIG_NETWORKING
void network_task_entry(void) {
    NetApp::init_wifi_and_dhcp("auroraOS_IoT", "88888888");
}
#endif

extern "C" {
    extern uint32_t _heap_start;
    extern uint32_t _heap_end;
}

Mutex uart_mutex;

#include "uart_device.hpp"
#include "procfs.hpp"

#include "power_manager.hpp" // 引入电源管理器

// ==========================================
// 绝对最低优先级的空闲任务 (Priority = 0)
// 只有当所有业务、网络、渲染任务都在睡觉时，它才会被调度执行
// ==========================================
void idle_task_entry(void) {
    int console_fd = open("/dev/uart0", 0);
    write(console_fd, "[Power] Idle Task Online. Tickless Engine Active.\n", 50);
    close(console_fd);

    while (true) {
        // 1. 向调度器询问：我们距离下一个任务醒来还有多久？
        uint32_t expected_idle = Scheduler::instance().get_expected_idle_ticks();

        // 2. 将预测时间交给电源管理器，由它决定是浅睡还是彻底关停 SysTick (Tickless)
        if (expected_idle > 0 && expected_idle != 0xFFFFFFFF) {
            PowerManager::instance().on_tick(expected_idle);
            PowerManager::instance().execute_wfi_if_needed();
        }
    }
}

extern "C" void shell_task(void) {
    // 1. 预设之前写的 log.txt
    int fd = VfsManager::instance().open("/tmp/log.txt");
    if (fd >= 0) {
        const char* secret = "Hello from auroraOS RamFS! You found the hidden message.";
        int len = 0; while (secret[len]) len++;
        VfsManager::instance().write(fd, secret, len);
        VfsManager::instance().close(fd);
    }

    // 2. 【极其硬核】我们在内存中手写构建一个真实的 100 字节 ARM Thumb 可执行 ELF 文件！
    // 它包含了一个正常的 Elf32_Ehdr, Elf32_Phdr 以及一段执行 SVC #0x01 系统调用的机器码！
    static const unsigned char mini_arm_elf[] = {
        // --- 1. Elf32_Ehdr (52 Bytes) ---
        0x7f, 'E', 'L', 'F', 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x28, 0x00, 0x01, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x00, 0x20, 0x00, 0x01, 0x00, 0x28, 0x00,
        0x00, 0x00, 0x00, 0x00,
        // --- 2. Elf32_Phdr (32 Bytes) ---
        0x01, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x1a, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        // --- 3. 真实机器码 payload (26 Bytes) ---
        // 汇编意义：将 PC+4 处的数据字符串地址放入 R0，然后执行 SVC #0x01，最后死循环休眠
        0x01, 0xa0, 0x01, 0xdf, 0xfe, 0xe7, 0x00, 0x00, 
        // 字符串内容: "DYNAMIC ELF OK!"
        'D', 'Y', 'N', 'A', 'M', 'I', 'C', ' ', 'E', 'L', 'F', ' ', 'O', 'K', '!', '\r', '\n', '\0'
    };

    int elf_fd = VfsManager::instance().open("/tmp/app.elf");
    if (elf_fd >= 0) {
        VfsManager::instance().write(elf_fd, reinterpret_cast<const char*>(mini_arm_elf), sizeof(mini_arm_elf));
        VfsManager::instance().close(elf_fd);
    }

    // 3. 启动命令行终端
    Shell::run();
}

#include "mutex.hpp"
Mutex pi_lock;

void pi_test_low() {
    Scheduler::instance().sleep_ms(500); // 错开系统启动的日志打印期
    sys_print("\r\n[Low] Task started, grabbing lock...\r\n");
    pi_lock.lock();
    sys_print("[Low] Lock acquired. Sleeping to let Mid & High wake up...\r\n");
    Scheduler::instance().sleep_ms(500); 
    // 此时它被唤醒，如果 PI 成功，它的优先级已经被 High 拔高，它能抢占 Mid 运行！
    sys_print("[Low] Woken up with inherited priority. Releasing lock...\r\n");
    pi_lock.unlock();
    sys_print("[Low] Lock released. Base priority restored.\r\n");
    while (1) Scheduler::instance().sleep_ms(10000);
}

void pi_test_mid() {
    Scheduler::instance().sleep_ms(700); // 等 Low 先拿到锁
    sys_print("[Mid] Task woken up! Starting busy loop to starve Low...\r\n");
    // 疯狂循环模拟 CPU 占用，注意不能加 volatile 防止被优化没，而是加一点实际工作或者 volatile 计数
    for (volatile int i = 0; i < 50000000; i++) {}
    sys_print("[Mid] Busy loop finished. If PI worked, this prints AFTER High gets the lock.\r\n");
    while (1) Scheduler::instance().sleep_ms(10000);
}

void pi_test_high() {
    Scheduler::instance().sleep_ms(800); // 等 Mid 开始疯狂占用 CPU 后再醒来
    sys_print("[High] Task woken up! Trying to grab lock...\r\n");
    pi_lock.lock();
    sys_print("[High] Lock acquired! Priority Inheritance SUCCESS!\r\n");
    pi_lock.unlock();
    while (1) Scheduler::instance().sleep_ms(10000);
}

#include "timer.hpp"
#include "posix.hpp"
#include "work_queue.hpp"

#ifdef CONFIG_WORK_QUEUE
// 工作队列守护线程的入口包裹函数
void workqueue_daemon_entry(void) {
    WorkQueue::instance().worker_task();
}
#endif

#ifdef CONFIG_TIMER_MANAGER
// 定时器守护线程的入口包裹函数
void timer_daemon_entry(void) {
    TimerManager::instance().daemon_task();
}

// 用户回调：定时器到期时执行
void my_timer_callback(void* arg) {
    int fd = open("/dev/uart0", 0);
    if (fd >= 0) {
        write(fd, "\r\n[Timer Callback] Software Timer Triggered asynchronously!\r\n", 61);
        close(fd);
    }
}

void posix_app_task(void) {
    int fd = open("/dev/uart0", 0);
    if (fd >= 0) {
        // 创建一个 2000 毫秒（2秒）周期触发的软件定时器
        TimerManager::instance().start_timer(2000, TimerType::Periodic, my_timer_callback);
        write(fd, "\r\n[App] Software Timer (2s) scheduled.\r\n", 40);

        while (1) {
            write(fd, "[App] Main app loop running...\r\n", 32);
            Scheduler::instance().sleep_ms(3000); // 故意睡 3 秒，和定时器的 2 秒产生异步交错
        }
    }
    while (1) { Scheduler::instance().sleep_ms(10000); } 
}
#endif

// =========================================================================
// [核心系统进程] 任务通知与 POSIX 信号测试应用
// ==========================================
static uint32_t g_receiver_task_id = 0xFFFFFFFF;

void receiver_task(void) {
    int fd = open("/dev/uart0", 0);

    // 1. 绑定 POSIX SIGUSR1 信号的异步回调
    signal(SIGUSR1, [](int sig) {
        int fd = open("/dev/uart0", 0);
        const char msg[] = "\r\n>>> [POSIX Signal Handler] SIGUSR1 intercepted asynchronously! <<<\r\n";
        write(fd, msg, sizeof(msg) - 1);
        close(fd);
    });

    write(fd, "[Receiver] Ready and waiting for zero-overhead Task Notifications...\r\n", 70);
    close(fd);

    while (true) {
        // 2. 0 内存开销、0 耗时等待任务通知
        uint32_t val = TaskNotify::take(); 
        
        fd = open("/dev/uart0", 0);
        write(fd, "[Receiver] Task Notification Received! Value: 0x", 48);
        
        // 简单以十六进制打印输出
        char hex_str[11];
        for (int i = 7; i >= 0; i--) {
            int nibble = (val >> (i * 4)) & 0xF;
            hex_str[7 - i] = nibble < 10 ? ('0' + nibble) : ('A' + nibble - 10);
        }
        hex_str[8] = '\r';
        hex_str[9] = '\n';
        hex_str[10] = '\0';
        write(fd, hex_str, 10);
        close(fd);
    }
}

void sender_task(void) {
    Scheduler::instance().sleep_ms(1500); // 等待接收线程就绪

    while (true) {
        // 测试 1：发送 FreeRTOS 任务通知
        TaskNotify::give(g_receiver_task_id, 0xA5A5);
        Scheduler::instance().sleep_ms(2000);

        // 测试 2：跨线程发送 POSIX 异步软件信号
        kill(g_receiver_task_id, SIGUSR1);
        Scheduler::instance().sleep_ms(2000);
    }
}

// 1. 实例化全局 128x128 智能手表 OLED 驱动与显存缓冲
OledDriver g_oled("oled0", 128, 128);
FrameBuffer<128, 128> g_fb;

// 2. 实例化全局 I2C 触控屏驱动，命名为 touch0
TouchDriver g_touch("touch0", 128, 128);

HeartRateSensor g_health_sensor;
WatchFaceEngine g_watchface;

// 可拖拽的 UI 控件组件 (比如一个 24x24 的手表智能应用卡片)
struct DraggableWidget {
    uint16_t    x, y;
    uint16_t    width, height;
    ColorRGB565 color;
    bool        is_dragging;
};

// 前向声明：供 ui_render_task 判断前台状态
extern AppControlBlock g_lua_app;
extern AppControlBlock g_fitness_app;

// ==========================================
// 手表主 UI 界面与拖拽交互引擎 + 表盘小组件引擎
// ==========================================
enum class WatchPage : uint8_t {
    WATCH_FACE,
    HEART_RATE,
    ACTIVITY
};

void ui_render_task(void) {
    g_oled.open();
    
    // 打开触控屏驱动，获得 POSIX 文件描述符！
    int touch_fd = open("/dev/touch0", 0);
    int console_fd = open("/dev/uart0", 0);
    
    write(console_fd, "\r\n⌚ [auroraOS] WatchFace, Input Engine & Sensor Framework Online. Phase 2 Complete!\r\n", 87);
    close(console_fd);

    // 配置表盘上的两个数据挂载槽位 (对标 watchOS Complications)
    g_watchface.add_complication(10, 10, 50, 20, 0xF800, 0x0000, hr_data_provider);
    g_watchface.add_complication(65, 10, 50, 20, 0x07E0, 0x0000, step_data_provider);

    GestureRecognizer recognizer;
    WatchPage current_page = WatchPage::WATCH_FACE;
    uint32_t simulated_tick = 0;

    // 第一帧：绘制背景并刷新
    g_fb.clear(0x0000);
    g_fb.flush(g_oled);
    
    FrameSchedulerV2::instance().wait_for_next_frame();

    while (true) {
        // 如果任何应用已切到前台（如 Lua 小程序），则让出 g_fb 避免竞争撕裂
        if (g_lua_app.state == AppState::FOREGROUND ||
            g_fitness_app.state == AppState::FOREGROUND) {
            FrameSchedulerV2::instance().wait_for_next_frame();
            continue;
        }

        // --- 1. 处理触摸交互与手势识别 ---
        TouchPoint touch;
        int bytes = read(touch_fd, reinterpret_cast<char*>(&touch), sizeof(TouchPoint));
        
        simulated_tick += 33; // 假设每帧 V-Sync 消耗 33ms

        GestureType gesture = GestureType::NONE;
        if (bytes == sizeof(TouchPoint) && touch.is_valid) {
            RawTouchEvent ev = { touch.x, touch.y, touch.state, simulated_tick };
            GestureEvent ge = recognizer.process_event(ev);
            gesture = ge.type;
        }

        // --- 2. 页面路由状态机切换 ---
        if (gesture == GestureType::SWIPE_LEFT) {
            console_fd = open("/dev/uart0", 0);
            write(console_fd, "\r\n👈 [Gesture Event] Swipe LEFT! Switch to next page.\r\n", 57);
            close(console_fd);

            if (current_page == WatchPage::WATCH_FACE) current_page = WatchPage::HEART_RATE;
            else if (current_page == WatchPage::HEART_RATE) current_page = WatchPage::ACTIVITY;
            else if (current_page == WatchPage::ACTIVITY) current_page = WatchPage::WATCH_FACE;

            g_fb.clear(0x0000); // 换页时清空屏幕缓冲区
        } else if (gesture == GestureType::SWIPE_RIGHT) {
            console_fd = open("/dev/uart0", 0);
            write(console_fd, "\r\n👉 [Gesture Event] Swipe RIGHT! Switch to previous page.\r\n", 62);
            close(console_fd);

            if (current_page == WatchPage::WATCH_FACE) current_page = WatchPage::ACTIVITY;
            else if (current_page == WatchPage::ACTIVITY) current_page = WatchPage::HEART_RATE;
            else if (current_page == WatchPage::HEART_RATE) current_page = WatchPage::WATCH_FACE;

            g_fb.clear(0x0000); // 换页时清空屏幕缓冲区
        }

        // --- 3. 页面内容渲染 ---
        if (current_page == WatchPage::WATCH_FACE) {
            // 3.1 主表盘页：渲染大字时间 "10:09" + 两侧小组件 (Complications)
            FontEngine::draw_string(20, 50, "10:09", FontColor::WHITE, FontSize::EXTRA_LARGE, g_fb.get_raw_buffer(), 128);
            g_watchface.render(g_fb);
        } 
        else if (current_page == WatchPage::HEART_RATE) {
            // 3.2 实时心率测量页
            FontEngine::draw_string(20, 20, "HEART RATE", FontColor::RED, FontSize::SMALL, g_fb.get_raw_buffer(), 128);
            
            SensorData data;
            uint32_t bpm = 0;
            // 尝试读取 SensorManager 的心率数据
            if (SensorManager::instance().pop_data(&data) && data.type == SensorType::HEART_RATE) {
                bpm = data.payload.bpm;
            } else {
                SensorManager::instance().get_hr_sensor().read(&data);
                bpm = data.payload.bpm;
            }
            
            FontEngine::draw_number(35, 60, bpm, FontColor::WHITE, FontSize::EXTRA_LARGE, g_fb.get_raw_buffer(), 128);
            FontEngine::draw_string(85, 80, "bpm", FontColor::GRAY, FontSize::SMALL, g_fb.get_raw_buffer(), 128);
        } 
        else if (current_page == WatchPage::ACTIVITY) {
            // 3.3 运动计步数据页
            FontEngine::draw_string(30, 20, "ACTIVITY", FontColor::GREEN, FontSize::SMALL, g_fb.get_raw_buffer(), 128);
            
            uint32_t steps = SensorManager::instance().get_accel_sensor().get_steps();
            
            FontEngine::draw_number(20, 60, steps, FontColor::WHITE, FontSize::EXTRA_LARGE, g_fb.get_raw_buffer(), 128);
            FontEngine::draw_string(80, 80, "steps", FontColor::GRAY, FontSize::SMALL, g_fb.get_raw_buffer(), 128);
        }

        // --- 4. 动态合围脏区域刷新同步到 OLED 屏幕 ---
        g_fb.flush(g_oled);

        // --- 5. 遵守 30FPS V-Sync
        FrameSchedulerV2::instance().wait_for_next_frame();
    }
}

// ==========================================
// 2. 后台健康传感器数据处理 (NORMAL 帧间执行)
// ==========================================
void sensor_log_task(void) {
    while (true) {
        int fd = open("/dev/uart0", 0);
        const char msg[] = "        ⚙️ [Inter-Frame] Background Sensor Log Running in 21ms gap!\r\n";
        write(fd, msg, sizeof(msg) - 1);
        close(fd);

        // 模拟较长的传感器卡尔曼滤波数学运算
        for (volatile int i = 0; i < 400000; i++);
        
        Scheduler::instance().sleep_ms(10); // 稍微出让一下，让打印更工整
    }
}

// 1. 全局实例化存储子系统三级流水线
FlashBlockDevice g_nor_flash("spiflash0", 4096, 128); // 512KB 闪存
PhotonCacheLayer g_photon_cache(g_nor_flash);         // 蓝河光子缓存层
LittleFsAdapter  g_lfs(g_photon_cache, 4096, 128);    // LittleFS 日志文件系统
LittleFsVNode    g_vfs_lfs(g_lfs);                    // LittleFS VFS 挂载节点

// aurora_get_time 实现：供 Lua 小程序引擎调用
void aurora_get_time(uint32_t& h, uint32_t& m) {
    uint32_t ticks = TimerManager::instance().get_current_tick();
    uint32_t total_seconds = ticks / 1000;
    h = (total_seconds / 3600) % 24;
    m = (total_seconds / 60) % 60;
}

// ==========================================
// Phase 3: Lua 小程序引擎与生命周期守护任务
// ==========================================
AppControlBlock g_fitness_app = {0, AppState::NOT_RUNNING, "FitnessTracker"};
AppControlBlock g_lua_app = {0, AppState::NOT_RUNNING, "LuaFitness"};
MiniProgramEngine g_lua_engine;

void system_daemon_task(void) {
    int console_fd = open("/dev/uart0", 0);
    write(console_fd, "\r\n[AI] Intent Engine & App Lifecycle Manager Online.\r\n", 54);
    close(console_fd);

#ifdef CONFIG_TIMER_MANAGER
    TimerManager::instance().start_timer(1000, TimerType::Periodic, [](void*) {
        g_photon_cache.sync();
    });
#ifdef CONFIG_NETWORKING
    TimerManager::instance().start_timer(5000, TimerType::Periodic, [](void*) {
        DeviceRouteTable::instance().dump_routes();
    });
#endif
#endif

#ifdef CONFIG_NETWORKING
    IntentEngine::Context intent_ctx;
    while (true) {
        IntentEngine::process_sensors(g_lua_app, intent_ctx);
        Scheduler::instance().sleep_ms(500);
    }
#else
    while (true) {
        Scheduler::instance().sleep_ms(500);
    }
#endif
}

const char* sample_fitness_app = R"(
    -- auroraOS 小程序生命周期函数：启动时调用
    function on_start()
        aurora.print("Fitness Mini-App Started!")
        -- 画一个深灰色的全屏背景
        aurora.fill_rect(0, 0, 128, 128, 0x18E3) 
    end

    -- auroraOS 小程序生命周期函数：每帧刷新时调用
    function on_update()
        local hr = aurora.get_heart_rate()
        aurora.print("Current HR read by Lua: " .. tostring(hr))
        
        -- 根据心率的数值动态绘制红色的心跳柱状图
        local bar_height = hr
        if bar_height > 100 then bar_height = 100 end
        
        -- 先用黑色擦除旧的柱状图区域
        aurora.fill_rect(50, 20, 20, 100, 0x0000)
        
        -- 画出新的红色柱形 (从底部向上画)
        local y_pos = 128 - bar_height
        aurora.fill_rect(50, y_pos, 20, bar_height, 0xF800)
    end
)";

void lua_app_task(void) {
    // 1. 初始化引擎并加载外部脚本
    if (g_lua_engine.init()) {
        g_lua_engine.load_app(sample_fitness_app);
        
        // 触发初始化钩子
        g_lua_engine.call_hook("on_start");
        g_lua_app.transition_to(AppState::FOREGROUND);
    }

    while (true) {
        // 2. 只有前台应用才有资格调用帧刷新函数
        if (g_lua_app.state == AppState::FOREGROUND) {
            
            // 将控制权移交给 Lua 脚本执行其内部业务逻辑！
            g_lua_engine.call_hook("on_update");
            
            // 将 Lua 画出的脏区域推送到物理 OLED 屏幕
            g_fb.flush(g_oled);
        }

        // 3. 遵守 30FPS 的帧感知调度，将剩余 CPU 时间让出
        FrameSchedulerV2::instance().wait_for_next_frame();
    }
}

// ==========================================
// 模拟手表高频写日志任务 (验证光子缓冲写聚合)
// ==========================================
void storage_test_task(void) {
    int console_fd = open("/dev/uart0", 0);
    write(console_fd, "\r\n[BlueOS Storage] LittleFS & Photon Cache Layer Online.\r\n", 58);

    // 1. 挂载 LittleFS 日志文件系统
    if (g_lfs.mount()) {
        write(console_fd, "[LittleFS] Mounted successfully over Flash Block Device.\r\n", 58);
    }

    // 2. 将整个文件系统挂载到 VFS 路径
    VfsManager::instance().mount("/storage", &g_vfs_lfs);
    write(console_fd, "[VFS] Mounted /storage to LFS VNode.\r\n\r\n", 40);

    // 3. 模拟高频微小字节写入（连续记录 5 次运动传感器数据）
    int log_fd = VfsManager::instance().open("/storage/steps.log", O_CREAT | O_WRONLY | O_APPEND);
    if (log_fd >= 0) {
        for (int i = 1; i <= 5; i++) {
            char log_entry[64];
            int len = 0;
            auto append = [&](const char* s) { while (*s) log_entry[len++] = *s++; };
            auto append_num = [&](int n) { log_entry[len++] = '0' + n; };

            append("[Record #"); append_num(i); append("] HeartRate: 128bpm, Step: 8642\n");
            
            // 每次仅写入微小的 42 个字节！
            write(log_fd, log_entry, len);
            
            write(console_fd, "  ⚡ [Photon Cache] Intercepted 42B write. Aggregated in RAM (0 Flash Erase!)\r\n", 80);
            Scheduler::instance().sleep_ms(1000);
        }

        write(console_fd, "\r\n🔒 [Sync] Explicit sync triggered. Flushing dirty RAM pages to Flash...\r\n", 77);
        g_photon_cache.sync(); // 触发全量物理落盘
        
        // 4. 读取校验持久化数据
        write(console_fd, "\r\n📖 --- Reading Back from LittleFS Persistent Storage --- 📖\r\n", 67);
        VfsManager::instance().lseek(log_fd, 0, 0); // 0 corresponds to SEEK_SET
        
        char read_buf[256];
        int bytes_read = read(log_fd, read_buf, sizeof(read_buf) - 1);
        if (bytes_read > 0) {
            read_buf[bytes_read] = '\0';
            write(console_fd, read_buf, bytes_read);
        }
        close(log_fd);
    }

    while (1) { Scheduler::instance().sleep_ms(10000); }
}

// =========================================================================
// [核心系统进程] 黑客应用任务
// ==========================================
void hacker_app_task(void) {
    sys_print("\r\n[Hacker App] Attempting to crack kernel security...\r\n");

    // 尝试一：通过系统调用合法打印（正常通过）
    sys_print("[Hacker App] Step 1: Legal syscall works fine.\r\n");

    Scheduler::instance().sleep_ms(3000); // 延时3秒，确保前面系统的启动日志能完整打印出来

    // 此时主动将自身的 CPU 特权级降级为 Unprivileged (普通应用态)
    sys_print("[Hacker App] Dropping CPU privilege level to User Mode...\r\n");
    {
        uint32_t ctrl;
        __asm__ volatile ("mrs %0, control" : "=r"(ctrl));
        ctrl |= 1u;  // Set nPRIV bit -> Unprivileged
        __asm__ volatile ("msr control, %0" :: "r"(ctrl) : "memory");
        __asm__ volatile ("nop");  // M0+ has no ISB
    }

    // 尝试二：恶意构造一个指向内核核心变量的指针，试图修改系统的 Tick！
    sys_print("[Hacker App] Step 2: Attempting illegal write to kernel tick_count...\r\n");
    
    extern volatile uint32_t tick_count;
    tick_count = 0xDEADBEEF; // 这一行一旦执行，触发 MPU MemManage！

    // 永远不会执行到这一步！
    sys_print("[Hacker App] Oh no! System hacked!\r\n"); 
}

// 分配给 hacker_app_task 的栈，大小必须是 2 的幂次方且地址对齐
alignas(1024) uint8_t hacker_stack[1024];

extern "C" void kernel_main(void) {
    uart_init();
    KernelHeap::instance().init(&_heap_start, &_heap_end);

    // ==========================================
    // 激活 MPU 空间隔离安全防火墙
    // ==========================================
    MPU::instance().disable();

    // 1. 保护 Flash 代码区 (假设从 0x00000000 开始，大小 256KB = 2^18)
    // 权限：全系统只读 (AP_ALL_RO)，允许执行代码
    MPU::instance().configure_region(0, 0x00000000, 18, MPU::AP_ALL_RO, false);

    // 2. 锁死全局 RAM 内存空间 (假设从 0x20000000 开始，大小 64KB = 2^16)
    // 权限：仅内核特权态读写 (AP_PRIV_RW)，严禁用户态触碰！
    MPU::instance().configure_region(1, 0x20000000, 16, MPU::AP_PRIV_RW, true);

    MPU::instance().enable();
    sys_print("[Security] MPU Memory Protection Unit Activated.\r\n");

    VfsManager::instance().init();
#ifdef CONFIG_NETWORKING
    auroraos::ble::BleSignatureVerifier::instance().init();
#endif

#ifdef CONFIG_FS_RAMFS
    static RamFile temp_file(1024);
    static RamFile elf_file(1024);
    VfsManager::instance().mount("/tmp/log.txt", &temp_file);
    VfsManager::instance().mount("/tmp/app.elf", &elf_file);
#endif

#ifdef CONFIG_DEVICE_UART
    // 挂载 设备 和 /tmp 目录下的虚拟文件
    static UartDevice uart0_dev("uart0");
    DeviceRegistry::instance().register_device(&uart0_dev);
#endif
    DeviceRegistry::instance().register_device(&g_oled);
    DeviceRegistry::instance().register_device(&g_touch);
    DeviceRegistry::instance().register_device(&g_nor_flash);
    // DeviceRegistry::instance().register_device(&g_health_sensor);
    
#ifdef CONFIG_FS_PROCFS
    // Mount ProcFS nodes
    static MemInfoNode meminfo_node;
    VfsManager::instance().mount("/proc/meminfo", &meminfo_node);
    static TaskInfoNode taskinfo_node;
    VfsManager::instance().mount("/proc/taskinfo", &taskinfo_node);
    static LatencyNode latency_node;
    VfsManager::instance().mount("/proc/latency", &latency_node);
    static PowerNode power_node;
    VfsManager::instance().mount("/proc/power", &power_node);
    static NetNode net_node;
    VfsManager::instance().mount("/proc/net", &net_node);
    static SoftbusNode softbus_node;
    VfsManager::instance().mount("/proc/softbus", &softbus_node);
#endif
    
    // 初始化调度器
    Scheduler& sched = Scheduler::instance();
    sched.init();

    // ── 任务优先级分配表 ──────────────────────────────────────────
    // sys_idle_task   : Idle     — CPU 兜底进程，永不休眠
    // shell_task      : High     — 交互终端，响应键盘输入
    // lwIP net tasks  : Realtime — 网络 RX 数据泵（在 tcpip_init_done 中创建）
    // udp_echo_task   : Normal   — 业务层 Echo 处理
    // ─────────────────────────────────────────────────────────────
    constexpr uint32_t STACK_SIZE_IDLE = 128;
    constexpr uint32_t STACK_SIZE_SHELL = 512;
    constexpr uint32_t STACK_SIZE_TEST = 128;
    constexpr uint32_t STACK_SIZE_DAEMON = 256;

    static uint32_t idle_stack[STACK_SIZE_IDLE];
    static uint32_t shell_stack[STACK_SIZE_SHELL];

    // 1. 空闲进程：优先级最低，负责 CPU 低功耗兜底
    if (!Scheduler::instance().create_task(idle_task_entry, idle_stack, STACK_SIZE_IDLE * sizeof(uint32_t),
        TaskPriority::Idle)) {
        sys_print("[Kernel] FATAL: failed to spawn idle_task_entry!\r\n");
    }

    // 2. 交互终端：高优先级响应用户键盘
    extern void shell_task(void);
    if (!Scheduler::instance().create_task(shell_task, shell_stack, STACK_SIZE_SHELL * sizeof(uint32_t),
        TaskPriority::High)) {
        sys_print("[Kernel] FATAL: failed to spawn shell_task!\r\n");
    }

    // 3. PI Mutex 测试任务
    static uint32_t pi_low_stack[STACK_SIZE_TEST];
    static uint32_t pi_mid_stack[STACK_SIZE_TEST];
    static uint32_t pi_high_stack[STACK_SIZE_TEST];
    Scheduler::instance().create_task(pi_test_low, pi_low_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::Low);
    Scheduler::instance().create_task(pi_test_mid, pi_mid_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::Normal);
    Scheduler::instance().create_task(pi_test_high, pi_high_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::High);

    // 4. Hacker App Task (带有 MPU 沙盒隔离保护的测试线程)
    Scheduler::instance().create_task(hacker_app_task, reinterpret_cast<uint32_t*>(hacker_stack), sizeof(hacker_stack), TaskPriority::Low, 10);

    // 5. Task Notify & POSIX Signal Test Tasks
    static uint32_t rx_stack[STACK_SIZE_TEST];
    static uint32_t tx_stack[STACK_SIZE_TEST];
    TaskControlBlock* rx_tcb = Scheduler::instance().create_task(receiver_task, rx_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::Normal);
    if (rx_tcb) g_receiver_task_id = rx_tcb->id;
    Scheduler::instance().create_task(sender_task, tx_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::Normal);

    // 6. 蓝河 Frame-Aware Scheduler 任务注册
    static uint32_t ui_stack[STACK_SIZE_TEST];
    uint32_t ui_tid = FrameSchedulerV2::instance().create_frame_task(ui_render_task, ui_stack, STACK_SIZE_TEST * sizeof(uint32_t), TaskPriority::Realtime);

    static uint32_t sensor_stack[STACK_SIZE_TEST];
    FrameSchedulerV2::instance().create_frame_task(sensor_log_task, sensor_stack, STACK_SIZE_TEST * sizeof(uint32_t), TaskPriority::Normal);

    // 7. 光子存储写聚合测试任务
    static uint32_t storage_stack[STACK_SIZE_SHELL];
    Scheduler::instance().create_task(storage_test_task, storage_stack, STACK_SIZE_SHELL * sizeof(uint32_t), TaskPriority::Normal);

    // 8. Phase 3: AI 意图引擎守护进程与 Lua 小程序
    static uint32_t daemon_stack[STACK_SIZE_DAEMON];
    Scheduler::instance().create_task(system_daemon_task, daemon_stack, STACK_SIZE_DAEMON * sizeof(uint32_t), TaskPriority::High);
    
    // Lua 虚拟机需要较大的栈
    static uint32_t lua_stack[2048];
    uint32_t tid_lua = FrameSchedulerV2::instance().create_frame_task(
        lua_app_task, lua_stack, 2048 * sizeof(uint32_t), TaskPriority::Realtime
    );
    g_lua_app.tid = tid_lua;

    // 【蓝河引擎绑定】初始化 30FPS 调度器，并绑定 UI 主任务的 ID
    FrameSchedulerV2::instance().init(30, ui_tid);

#ifdef CONFIG_TIMER_MANAGER
    // 4. 定时器守护进程与测试 App
    static uint32_t timer_daemon_stack[STACK_SIZE_DAEMON];
    static uint32_t posix_app_stack[STACK_SIZE_DAEMON];
    Scheduler::instance().create_task(timer_daemon_entry, timer_daemon_stack, STACK_SIZE_DAEMON*sizeof(uint32_t), TaskPriority::Realtime);
    Scheduler::instance().create_task(posix_app_task, posix_app_stack, STACK_SIZE_DAEMON*sizeof(uint32_t), TaskPriority::Low);
#endif

#ifdef CONFIG_WORK_QUEUE
    // 5. 工作队列守护进程 (使用 High 优先级)
    static uint32_t workq_daemon_stack[STACK_SIZE_DAEMON];
    Scheduler::instance().create_task(workqueue_daemon_entry, workq_daemon_stack, STACK_SIZE_DAEMON*sizeof(uint32_t), TaskPriority::High);
#endif

#ifdef CONFIG_NETWORKING
    // 【新增】创建独立的网络 DHCP 客户端线程
    static uint32_t net_stack[1024]; // lwIP 比较吃栈，给大一点 (4KB)
    sched.create_task(network_task_entry, net_stack, 1024 * sizeof(uint32_t), TaskPriority::Realtime);
#endif

    // 启动调度器：正确引导第一个任务（通过 PSP/bx 跳入，不破坏栈帧）
    // 调度器从此接管 CPU，永不返回
    Scheduler::instance().start();
}
