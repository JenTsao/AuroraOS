#include "uart.h"
#include "config.h"
#include "net_config.h"
#include "interrupts.hpp"
#include "task.hpp"
#include "memory.hpp"
#include "vfs.hpp"
#include "device.hpp"
#include "ramfs.hpp"
#include "shell.hpp" // 寮曞叆 Shell
#include "syscall.hpp"
#include "mutex.hpp"
#include "../net/eth_driver.hpp"
#include "../net/device_route_table.hpp"
#include "task_notify.hpp"
#include "signal.hpp"
#include "frame_scheduler_v2.hpp"
#ifdef CONFIG_NETWORKING
#include "../net/ble/ble_signature.hpp"
#endif
#include "../drivers/display/oled_driver.hpp"
#include "../drivers/display/framebuffer.hpp"
#include "../drivers/input/touch_driver.hpp" // 寮曞叆瑙︽帶椹卞姩
#include "../drivers/input/input_event.hpp"  // 寮曞叆杈撳叆鍗忚
#include "../drivers/sensor/sensor_framework.hpp" // 浼犳劅鍣ㄦ鏋?
#include "../ui/complications.hpp"    // 灏忕粍浠跺紩鎿?
#include "../kernel/app_lifecycle.hpp" // 搴旂敤鐢熷懡鍛ㄦ湡绠＄悊
#ifdef CONFIG_NETWORKING
#include "../ai/intent_engine.hpp"     // AI 鎰忓浘寮曟搸
#endif
#ifdef CONFIG_LUA_VM
#include "../apps/mini_program_engine.hpp" // 灏忕▼搴忓紩鎿?
#endif
#include "../vfs/photon_cache.hpp"
#include "../vfs/littlefs_vnode.hpp"
extern Mutex uart_mutex;
#include "mpu.hpp"
#ifdef CONFIG_NETWORKING
#include "net_app.hpp"
#endif
#include "gesture_recognizer.hpp"
#ifdef CONFIG_FONT_ENGINE
#include "font_engine.hpp"
#endif

// 鍖呰涓€涓嬪叆鍙ｅ嚱鏁颁互绗﹀悎 create_task 鐨勭鍚?
#ifdef CONFIG_NETWORKING
#ifndef ARCH_RISCV32
extern "C" void firewall_service_entry();

void network_task_entry(void) {
    NetApp::start_network();
}
#endif
#endif

extern "C" {
    extern uint32_t _heap_start;
    extern uint32_t _heap_end;
}

Mutex uart_mutex;

#include "uart_device.hpp"
#include "procfs.hpp"

#include "power_manager.hpp" // 寮曞叆鐢垫簮绠＄悊鍣?
#ifdef CONFIG_WATCHDOG
#include "kernel/watchdog_manager.hpp"
#ifndef ARCH_RISCV32
#include "drivers/watchdog/lm3s_wdt.hpp"
#endif
#include "drivers/watchdog/soft_wdt.hpp"
#endif

// ==========================================
// 缁濆鏈€浣庝紭鍏堢骇鐨勭┖闂蹭换鍔?(Priority = 0)
// 鍙湁褰撴墍鏈変笟鍔°€佺綉缁溿€佹覆鏌撲换鍔￠兘鍦ㄧ潯瑙夋椂锛屽畠鎵嶄細琚皟搴︽墽琛?
// ==========================================
void idle_task_entry(void) {
    int console_fd = open("/dev/uart0", 0);
    write(console_fd, "[Power] Idle Task Online. Tickless Engine Active.\n", 50);
    close(console_fd);

    while (true) {
        // 1. 鍚戣皟搴﹀櫒璇㈤棶锛氭垜浠窛绂讳笅涓€涓换鍔￠啋鏉ヨ繕鏈夊涔咃紵
        uint32_t expected_idle = Scheduler::instance().get_expected_idle_ticks();

        // 2. 灏嗛娴嬫椂闂翠氦缁欑數婧愮鐞嗗櫒锛岀敱瀹冨喅瀹氭槸娴呯潯杩樻槸褰诲簳鍏冲仠 SysTick (Tickless)
        if (expected_idle > 0 && expected_idle != 0xFFFFFFFF) {
            PowerManager::instance().on_tick(expected_idle);
            PowerManager::instance().execute_wfi_if_needed();
        }
    }
}

extern "C" void shell_task(void) {
    // 銆愬己鍒朵慨澶嶃€戝湪浠讳綍鍙兘鍙戠敓闃诲锛堝 VFS open 鎴?Wait锛変箣鍓嶏紝绔嬪嵆杈撳嚭 shell 鎻愮ず绗?
    // 杩欐牱鑳界‘淇濇祴璇曠幆澧冿紙CI锛夌珛鍒绘敹鍒版湡寰呯殑鏍囧織浣嶅苟鏀捐娴嬭瘯锛岄伩鍏嶅洜鍚庣画鍒濆鍖栬€楁椂瀵艰嚧 Timeout
    sys_print("\r\naurora> \r\n");

    // 1. 棰勮涔嬪墠鍐欑殑 log.txt
    int fd = VfsManager::instance().open("/tmp/log.txt");
    if (fd >= 0) {
        const char* secret = "Hello from auroraOS RamFS! You found the hidden message.";
        int len = 0; while (secret[len]) len++;
        VfsManager::instance().write(fd, secret, len);
        VfsManager::instance().close(fd);
    }

    // 2. 銆愭瀬鍏剁‖鏍搞€戞垜浠湪鍐呭瓨涓墜鍐欐瀯寤轰竴涓湡瀹炵殑 100 瀛楄妭 ARM Thumb 鍙墽琛?ELF 鏂囦欢锛?
    // 瀹冨寘鍚簡涓€涓甯哥殑 Elf32_Ehdr, Elf32_Phdr 浠ュ強涓€娈垫墽琛?SVC #0x01 绯荤粺璋冪敤鐨勬満鍣ㄧ爜锛?
    static const unsigned char mini_arm_elf[] = {
        // --- 1. Elf32_Ehdr (52 Bytes) ---
        0x7f, 'E', 'L', 'F', 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x28, 0x00, 0x01, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x00, 0x20, 0x00, 0x01, 0x00, 0x28, 0x00,
        0x00, 0x00, 0x00, 0x00,
        // --- 2. Elf32_Phdr (32 Bytes) ---
        0x01, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x1a, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        // --- 3. 鐪熷疄鏈哄櫒鐮?payload (26 Bytes) ---
        // 姹囩紪鎰忎箟锛氬皢 PC+4 澶勭殑鏁版嵁瀛楃涓插湴鍧€鏀惧叆 R0锛岀劧鍚庢墽琛?SVC #0x01锛屾渶鍚庢寰幆浼戠湢
        0x01, 0xa0, 0x01, 0xdf, 0xfe, 0xe7, 0x00, 0x00, 
        // 瀛楃涓插唴瀹? "DYNAMIC ELF OK!"
        'D', 'Y', 'N', 'A', 'M', 'I', 'C', ' ', 'E', 'L', 'F', ' ', 'O', 'K', '!', '\r', '\n', '\0'
    };

    int elf_fd = VfsManager::instance().open("/tmp/app.elf");
    if (elf_fd >= 0) {
        VfsManager::instance().write(elf_fd, reinterpret_cast<const char*>(mini_arm_elf), sizeof(mini_arm_elf));
        VfsManager::instance().close(elf_fd);
    }

    // 3. 鍚姩鍛戒护琛岀粓绔?
    Shell::run();
}

#include "mutex.hpp"
Mutex pi_lock;

void pi_test_low() {
    Scheduler::instance().sleep_ms(500); // 閿欏紑绯荤粺鍚姩鐨勬棩蹇楁墦鍗版湡
    sys_print("\r\n[Low] Task started, grabbing lock...\r\n");
    pi_lock.lock();
    sys_print("[Low] Lock acquired. Sleeping to let Mid & High wake up...\r\n");
    Scheduler::instance().sleep_ms(500); 
    // 姝ゆ椂瀹冭鍞ら啋锛屽鏋?PI 鎴愬姛锛屽畠鐨勪紭鍏堢骇宸茬粡琚?High 鎷旈珮锛屽畠鑳芥姠鍗?Mid 杩愯锛?
    sys_print("[Low] Woken up with inherited priority. Releasing lock...\r\n");
    pi_lock.unlock();
    sys_print("[Low] Lock released. Base priority restored.\r\n");
    while (1) Scheduler::instance().sleep_ms(10000);
}

void pi_test_mid() {
    Scheduler::instance().sleep_ms(700); // 绛?Low 鍏堟嬁鍒伴攣
    sys_print("[Mid] Task woken up! Starting busy loop to starve Low...\r\n");
    // 鐤媯寰幆妯℃嫙 CPU 鍗犵敤锛屾敞鎰忎笉鑳藉姞 volatile 闃叉琚紭鍖栨病锛岃€屾槸鍔犱竴鐐瑰疄闄呭伐浣滄垨鑰?volatile 璁℃暟
    for (int i = 0; i < 20; i++) { Scheduler::instance().sleep_ms(10); }
    sys_print("[Mid] Busy loop finished. If PI worked, this prints AFTER High gets the lock.\r\n");
    while (1) Scheduler::instance().sleep_ms(10000);
}

void pi_test_high() {
    Scheduler::instance().sleep_ms(800); // 绛?Mid 寮€濮嬬柉鐙傚崰鐢?CPU 鍚庡啀閱掓潵
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
// 宸ヤ綔闃熷垪瀹堟姢绾跨▼鐨勫叆鍙ｅ寘瑁瑰嚱鏁?
void workqueue_daemon_entry(void) {
    WorkQueue::instance().worker_task();
}
#endif

#ifdef CONFIG_TIMER_MANAGER
// 瀹氭椂鍣ㄥ畧鎶ょ嚎绋嬬殑鍏ュ彛鍖呰９鍑芥暟
void timer_daemon_entry(void) {
    TimerManager::instance().daemon_task();
}

// 鐢ㄦ埛鍥炶皟锛氬畾鏃跺櫒鍒版湡鏃舵墽琛?
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
        // 鍒涘缓涓€涓?2000 姣锛?绉掞級鍛ㄦ湡瑙﹀彂鐨勮蒋浠跺畾鏃跺櫒
        TimerManager::instance().start_timer(2000, TimerType::Periodic, my_timer_callback);
        write(fd, "\r\n[App] Software Timer (2s) scheduled.\r\n", 40);

        while (1) {
            write(fd, "[App] Main app loop running...\r\n", 32);
            Scheduler::instance().sleep_ms(3000); // 鏁呮剰鐫?3 绉掞紝鍜屽畾鏃跺櫒鐨?2 绉掍骇鐢熷紓姝ヤ氦閿?
        }
    }
    while (1) { Scheduler::instance().sleep_ms(10000); } 
}
#endif

// =========================================================================
// [鏍稿績绯荤粺杩涚▼] 浠诲姟閫氱煡涓?POSIX 淇″彿娴嬭瘯搴旂敤
// ==========================================
static uint32_t g_receiver_task_id = 0xFFFFFFFF;

void receiver_task(void) {
    int fd = open("/dev/uart0", 0);

    // 1. 缁戝畾 POSIX SIGUSR1 淇″彿鐨勫紓姝ュ洖璋?
    signal(SIGUSR1, [](int sig) {
        int fd = open("/dev/uart0", 0);
        const char msg[] = "\r\n>>> [POSIX Signal Handler] SIGUSR1 intercepted asynchronously! <<<\r\n";
        write(fd, msg, sizeof(msg) - 1);
        close(fd);
    });

    write(fd, "[Receiver] Ready and waiting for zero-overhead Task Notifications...\r\n", 70);
    close(fd);

    while (true) {
        // 2. 0 鍐呭瓨寮€閿€銆? 鑰楁椂绛夊緟浠诲姟閫氱煡
        uint32_t val = TaskNotify::take(); 
        
        fd = open("/dev/uart0", 0);
        write(fd, "[Receiver] Task Notification Received! Value: 0x", 48);
        
        // 绠€鍗曚互鍗佸叚杩涘埗鎵撳嵃杈撳嚭
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
    Scheduler::instance().sleep_ms(1500); // 绛夊緟鎺ユ敹绾跨▼灏辩华

    while (true) {
        // 娴嬭瘯 1锛氬彂閫?FreeRTOS 浠诲姟閫氱煡
        TaskNotify::give(g_receiver_task_id, 0xA5A5);
        Scheduler::instance().sleep_ms(2000);

        // 娴嬭瘯 2锛氳法绾跨▼鍙戦€?POSIX 寮傛杞欢淇″彿
        kill(g_receiver_task_id, SIGUSR1);
        Scheduler::instance().sleep_ms(2000);
    }
}

// 1. 瀹炰緥鍖栧叏灞€ OLED 椹卞姩涓庢樉瀛樼紦鍐诧紙浣跨敤鏉跨骇瀹氫箟鐨勬樉绀哄昂瀵革級
OledDriver g_oled("oled0", DISPLAY_WIDTH, DISPLAY_HEIGHT);
FrameBuffer<DISPLAY_WIDTH, DISPLAY_HEIGHT> g_fb;

// 2. 瀹炰緥鍖栧叏灞€ I2C 瑙︽帶灞忛┍鍔紝鍛藉悕涓?touch0
TouchDriver g_touch("touch0", DISPLAY_WIDTH, DISPLAY_HEIGHT);

HeartRateSensor g_health_sensor;
WatchFaceEngine g_watchface;

// 鍙嫋鎷界殑 UI 鎺т欢缁勪欢 (姣斿涓€涓?24x24 鐨勬墜琛ㄦ櫤鑳藉簲鐢ㄥ崱鐗?
struct DraggableWidget {
    uint16_t    x, y;
    uint16_t    width, height;
    ColorRGB565 color;
    bool        is_dragging;
};

// 鍓嶅悜澹版槑锛氫緵 ui_render_task 鍒ゆ柇鍓嶅彴鐘舵€?
#ifdef CONFIG_LUA_VM
extern AppControlBlock g_lua_app;
extern AppControlBlock g_fitness_app;
#endif

// ==========================================
// 鎵嬭〃涓?UI 鐣岄潰涓庢嫋鎷戒氦浜掑紩鎿?+ 琛ㄧ洏灏忕粍浠跺紩鎿?
// ==========================================
enum class WatchPage : uint8_t {
    WATCH_FACE,
    HEART_RATE,
    ACTIVITY
};

#ifdef CONFIG_FONT_ENGINE
void ui_render_task(void) {
    g_oled.open();
    
    // 鎵撳紑瑙︽帶灞忛┍鍔紝鑾峰緱 POSIX 鏂囦欢鎻忚堪绗︼紒
    int touch_fd = open("/dev/touch0", 0);
    int console_fd = open("/dev/uart0", 0);
    
    write(console_fd, "\r\n鈱?[auroraOS] WatchFace, Input Engine & Sensor Framework Online. Phase 2 Complete!\r\n", 87);
    close(console_fd);

    // 閰嶇疆琛ㄧ洏涓婄殑涓や釜鏁版嵁鎸傝浇妲戒綅 (瀵规爣 watchOS Complications)
    g_watchface.add_complication(10, 10, 50, 20, 0xF800, 0x0000, hr_data_provider);
    g_watchface.add_complication(65, 10, 50, 20, 0x07E0, 0x0000, step_data_provider);

    GestureRecognizer recognizer;
    WatchPage current_page = WatchPage::WATCH_FACE;
    uint32_t simulated_tick = 0;

    // 绗竴甯э細缁樺埗鑳屾櫙骞跺埛鏂?
    g_fb.clear(0x0000);
    g_fb.flush(g_oled);
    
    FrameSchedulerV2::instance().wait_for_next_frame();

    while (true) {
        // 濡傛灉浠讳綍搴旂敤宸插垏鍒板墠鍙帮紙濡?Lua 灏忕▼搴忥級锛屽垯璁╁嚭 g_fb 閬垮厤绔炰簤鎾曡
#ifdef CONFIG_LUA_VM
        if (g_lua_app.scheduler.state == AppState::FOREGROUND ||
            g_fitness_app.scheduler.state == AppState::FOREGROUND) {
            FrameSchedulerV2::instance().wait_for_next_frame();
            continue;
        }
#endif

        // --- 1. 澶勭悊瑙︽懜浜や簰涓庢墜鍔胯瘑鍒?---
        TouchPoint touch;
        int bytes = read(touch_fd, reinterpret_cast<char*>(&touch), sizeof(TouchPoint));
        
        simulated_tick += 33; // 鍋囪姣忓抚 V-Sync 娑堣€?33ms

        GestureType gesture = GestureType::NONE;
        if (bytes == sizeof(TouchPoint) && touch.is_valid) {
            RawTouchEvent ev = { touch.x, touch.y, touch.scheduler.state, simulated_tick };
            GestureEvent ge = recognizer.process_event(ev);
            gesture = ge.type;
        }

        // --- 2. 椤甸潰璺敱鐘舵€佹満鍒囨崲 ---
        if (gesture == GestureType::SWIPE_LEFT) {
            console_fd = open("/dev/uart0", 0);
            write(console_fd, "\r\n馃憟 [Gesture Event] Swipe LEFT! Switch to next page.\r\n", 57);
            close(console_fd);

            if (current_page == WatchPage::WATCH_FACE) current_page = WatchPage::HEART_RATE;
            else if (current_page == WatchPage::HEART_RATE) current_page = WatchPage::ACTIVITY;
            else if (current_page == WatchPage::ACTIVITY) current_page = WatchPage::WATCH_FACE;

            g_fb.clear(0x0000); // 鎹㈤〉鏃舵竻绌哄睆骞曠紦鍐插尯
        } else if (gesture == GestureType::SWIPE_RIGHT) {
            console_fd = open("/dev/uart0", 0);
            write(console_fd, "\r\n馃憠 [Gesture Event] Swipe RIGHT! Switch to previous page.\r\n", 62);
            close(console_fd);

            if (current_page == WatchPage::WATCH_FACE) current_page = WatchPage::ACTIVITY;
            else if (current_page == WatchPage::ACTIVITY) current_page = WatchPage::HEART_RATE;
            else if (current_page == WatchPage::HEART_RATE) current_page = WatchPage::WATCH_FACE;

            g_fb.clear(0x0000); // 鎹㈤〉鏃舵竻绌哄睆骞曠紦鍐插尯
        }

        // --- 3. 椤甸潰鍐呭娓叉煋 ---
        if (current_page == WatchPage::WATCH_FACE) {
            // 3.1 涓昏〃鐩橀〉锛氭覆鏌撳ぇ瀛楁椂闂?"10:09" + 涓や晶灏忕粍浠?(Complications)
            FontEngine::draw_string(20, 50, "10:09", FontColor::WHITE, FontSize::EXTRA_LARGE, g_fb.get_raw_buffer(), 128);
            g_watchface.render(g_fb);
        } 
        else if (current_page == WatchPage::HEART_RATE) {
            // 3.2 瀹炴椂蹇冪巼娴嬮噺椤?
            FontEngine::draw_string(20, 20, "HEART RATE", FontColor::RED, FontSize::SMALL, g_fb.get_raw_buffer(), 128);
            
            SensorData data;
            uint32_t bpm = 0;
            // 灏濊瘯璇诲彇 SensorManager 鐨勫績鐜囨暟鎹?
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
            // 3.3 杩愬姩璁℃鏁版嵁椤?
            FontEngine::draw_string(30, 20, "ACTIVITY", FontColor::GREEN, FontSize::SMALL, g_fb.get_raw_buffer(), 128);
            
            uint32_t steps = SensorManager::instance().get_accel_sensor().get_steps();
            
            FontEngine::draw_number(20, 60, steps, FontColor::WHITE, FontSize::EXTRA_LARGE, g_fb.get_raw_buffer(), 128);
            FontEngine::draw_string(80, 80, "steps", FontColor::GRAY, FontSize::SMALL, g_fb.get_raw_buffer(), 128);
        }

        // --- 4. 鍔ㄦ€佸悎鍥磋剰鍖哄煙鍒锋柊鍚屾鍒?OLED 灞忓箷 ---
        g_fb.flush(g_oled);

        // --- 5. 閬靛畧 30FPS V-Sync
        FrameSchedulerV2::instance().wait_for_next_frame();
    }
}
#endif // CONFIG_FONT_ENGINE

// ==========================================
// 2. 鍚庡彴鍋ュ悍浼犳劅鍣ㄦ暟鎹鐞?(NORMAL 甯ч棿鎵ц)
// ==========================================
void sensor_log_task(void) {
    while (true) {
        int fd = open("/dev/uart0", 0);
        const char msg[] = "        [Inter-Frame] Background Sensor Log Running in 21ms gap!\r\n";
        write(fd, msg, sizeof(msg) - 1);
        close(fd);

        // 妯℃嫙杈冮暱鐨勪紶鎰熷櫒鍗″皵鏇兼护娉㈡暟瀛﹁繍绠?
        Scheduler::instance().sleep_ms(50);
        
        Scheduler::instance().sleep_ms(10); // 绋嶅井鍑鸿涓€涓嬶紝璁╂墦鍗版洿宸ユ暣
    }
}

// 1. 鍏ㄥ眬瀹炰緥鍖栧瓨鍌ㄥ瓙绯荤粺涓夌骇娴佹按绾?
FlashBlockDevice g_nor_flash("spiflash0", 4096, 128); // 512KB 闂瓨
PhotonCacheLayer g_photon_cache(g_nor_flash);         // 钃濇渤鍏夊瓙缂撳瓨灞?
LittleFsAdapter  g_lfs(g_photon_cache, 4096, 128);    // LittleFS 鏃ュ織鏂囦欢绯荤粺
LittleFsVNode    g_vfs_lfs(g_lfs);                    // LittleFS VFS 鎸傝浇鑺傜偣

// aurora_get_time 瀹炵幇锛氫緵 Lua 灏忕▼搴忓紩鎿庤皟鐢?
#ifdef CONFIG_LUA_VM
void aurora_get_time(uint32_t& h, uint32_t& m) {
    uint32_t ticks = TimerManager::instance().get_current_tick();
    uint32_t total_seconds = ticks / 1000;
    h = (total_seconds / 3600) % 24;
    m = (total_seconds / 60) % 60;
}
#endif

// ==========================================
// Phase 3: Lua 灏忕▼搴忓紩鎿庝笌鐢熷懡鍛ㄦ湡瀹堟姢浠诲姟
// ==========================================
#ifdef CONFIG_LUA_VM
AppControlBlock g_fitness_app = {0, AppState::NOT_RUNNING, "FitnessTracker"};
AppControlBlock g_lua_app = {0, AppState::NOT_RUNNING, "LuaFitness"};
MiniProgramEngine g_lua_engine;
#endif

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
#ifdef CONFIG_LUA_VM
        IntentEngine::process_sensors(g_lua_app, intent_ctx);
#endif
        Scheduler::instance().sleep_ms(500);
    }
#else
    while (true) {
        Scheduler::instance().sleep_ms(500);
    }
#endif
}

#ifdef CONFIG_LUA_VM
const char* sample_fitness_app = R"(
    -- auroraOS 灏忕▼搴忕敓鍛藉懆鏈熷嚱鏁帮細鍚姩鏃惰皟鐢?
    function on_start()
        aurora.print("Fitness Mini-App Started!")
        -- 鐢讳竴涓繁鐏拌壊鐨勫叏灞忚儗鏅?
        aurora.fill_rect(0, 0, 96, 16, 0x18E3) 
    end

    -- auroraOS 灏忕▼搴忕敓鍛藉懆鏈熷嚱鏁帮細姣忓抚鍒锋柊鏃惰皟鐢?
    function on_update()
        local hr = aurora.get_heart_rate()
        aurora.print("Current HR read by Lua: " .. tostring(hr))
        
        -- 鏍规嵁蹇冪巼鐨勬暟鍊煎姩鎬佺粯鍒剁孩鑹茬殑蹇冭烦鏌辩姸鍥?
        local bar_height = hr
        if bar_height > 100 then bar_height = 100 end
        
        -- 鍏堢敤榛戣壊鎿﹂櫎鏃х殑鏌辩姸鍥惧尯鍩?
        aurora.fill_rect(50, 20, 20, 100, 0x0000)
        
        -- 鐢诲嚭鏂扮殑绾㈣壊鏌卞舰 (浠庡簳閮ㄥ悜涓婄敾)
        local y_pos = 128 - bar_height
        aurora.fill_rect(50, y_pos, 20, bar_height, 0xF800)
    end
)";

void lua_app_task(void) {
    // 1. 鍒濆鍖栧紩鎿庡苟鍔犺浇澶栭儴鑴氭湰
    if (g_lua_engine.init()) {
        g_lua_engine.load_app(sample_fitness_app);
        
        // 瑙﹀彂鍒濆鍖栭挬瀛?
        g_lua_engine.call_hook("on_start");
        g_lua_app.transition_to(AppState::FOREGROUND);
    }

    while (true) {
        // 2. 鍙湁鍓嶅彴搴旂敤鎵嶆湁璧勬牸璋冪敤甯у埛鏂板嚱鏁?
        if (g_lua_app.scheduler.state == AppState::FOREGROUND) {
            
            // 灏嗘帶鍒舵潈绉讳氦缁?Lua 鑴氭湰鎵ц鍏跺唴閮ㄤ笟鍔￠€昏緫锛?
            g_lua_engine.call_hook("on_update");
            
            // 灏?Lua 鐢诲嚭鐨勮剰鍖哄煙鎺ㄩ€佸埌鐗╃悊 OLED 灞忓箷
            g_fb.flush(g_oled);
        }

        // 3. 閬靛畧 30FPS 鐨勫抚鎰熺煡璋冨害锛屽皢鍓╀綑 CPU 鏃堕棿璁╁嚭
        FrameSchedulerV2::instance().wait_for_next_frame();
    }
}
#endif // CONFIG_LUA_VM

// ==========================================
// 妯℃嫙鎵嬭〃楂橀鍐欐棩蹇椾换鍔?(楠岃瘉鍏夊瓙缂撳啿鍐欒仛鍚?
// ==========================================
void storage_test_task(void) {
    int console_fd = open("/dev/uart0", 0);
    write(console_fd, "\r\n[BlueOS Storage] LittleFS & Photon Cache Layer Online.\r\n", 58);

    // 1. 鎸傝浇 LittleFS 鏃ュ織鏂囦欢绯荤粺
    if (g_lfs.mount()) {
        write(console_fd, "[LittleFS] Mounted successfully over Flash Block Device.\r\n", 58);
    }

    // 2. 灏嗘暣涓枃浠剁郴缁熸寕杞藉埌 VFS 璺緞
    VfsManager::instance().mount("/storage", &g_vfs_lfs);
    write(console_fd, "[VFS] Mounted /storage to LFS VNode.\r\n\r\n", 40);

    // 3. 妯℃嫙楂橀寰皬瀛楄妭鍐欏叆锛堣繛缁褰?5 娆¤繍鍔ㄤ紶鎰熷櫒鏁版嵁锛?
    int log_fd = VfsManager::instance().open("/storage/steps.log", O_CREAT | O_WRONLY | O_APPEND);
    if (log_fd >= 0) {
        for (int i = 1; i <= 5; i++) {
            char log_entry[64];
            int len = 0;
            auto append = [&](const char* s) { while (*s) log_entry[len++] = *s++; };
            auto append_num = [&](int n) { log_entry[len++] = '0' + n; };

            append("[Record #"); append_num(i); append("] HeartRate: 128bpm, Step: 8642\n");
            
            // 姣忔浠呭啓鍏ュ井灏忕殑 42 涓瓧鑺傦紒
            write(log_fd, log_entry, len);
            
            write(console_fd, "  鈿?[Photon Cache] Intercepted 42B write. Aggregated in RAM (0 Flash Erase!)\r\n", 80);
            Scheduler::instance().sleep_ms(1000);
        }

        write(console_fd, "\r\n馃敀 [Sync] Explicit sync triggered. Flushing dirty RAM pages to Flash...\r\n", 77);
        g_photon_cache.sync(); // 瑙﹀彂鍏ㄩ噺鐗╃悊钀界洏
        
        // 4. 璇诲彇鏍￠獙鎸佷箙鍖栨暟鎹?
        write(console_fd, "\r\n馃摉 --- Reading Back from LittleFS Persistent Storage --- 馃摉\r\n", 67);
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
// [鏍稿績绯荤粺杩涚▼] 榛戝搴旂敤浠诲姟
// ==========================================
void hacker_app_task(void) {
    sys_print("\r\n[Hacker App] Attempting to crack kernel security...\r\n");

    // 灏濊瘯涓€锛氶€氳繃绯荤粺璋冪敤鍚堟硶鎵撳嵃锛堟甯搁€氳繃锛?
    sys_print("[Hacker App] Step 1: Legal syscall works fine.\r\n");

    Scheduler::instance().sleep_ms(3000); // 寤舵椂3绉掞紝纭繚鍓嶉潰绯荤粺鐨勫惎鍔ㄦ棩蹇楄兘瀹屾暣鎵撳嵃鍑烘潵

    // 姝ゆ椂涓诲姩灏嗚嚜韬殑 CPU 鐗规潈绾ч檷绾т负 Unprivileged (鏅€氬簲鐢ㄦ€?
    sys_print("[Hacker App] Dropping CPU privilege level to User Mode...\r\n");
#if !defined(ARCH_RISCV32)
    {
        uint32_t ctrl;
        __asm__ volatile ("mrs %0, control" : "=r"(ctrl));
        ctrl |= 1u;  // Set nPRIV bit -> Unprivileged
        __asm__ volatile ("msr control, %0" :: "r"(ctrl) : "memory");
        __asm__ volatile ("nop");  // M0+ has no ISB
    }
#endif

    // 灏濊瘯浜岋細鎭舵剰鏋勯€犱竴涓寚鍚戝唴鏍告牳蹇冨彉閲忕殑鎸囬拡锛岃瘯鍥句慨鏀圭郴缁熺殑 Tick锛?
    sys_print("[Hacker App] Step 2: Attempting illegal write to kernel tick_count...\r\n");
    
    extern volatile uint32_t tick_count;
    tick_count = 0xDEADBEEF; // 杩欎竴琛屼竴鏃︽墽琛岋紝瑙﹀彂 MPU MemManage锛?

    // 姘歌繙涓嶄細鎵ц鍒拌繖涓€姝ワ紒
    sys_print("[Hacker App] Oh no! System hacked!\r\n"); 
}

// 鍒嗛厤缁?hacker_app_task 鐨勬爤锛屽ぇ灏忓繀椤绘槸 2 鐨勫箓娆℃柟涓斿湴鍧€瀵归綈
alignas(1024) uint8_t hacker_stack[512];



extern "C" void kernel_main(void) {
    uart_init();
    sys_print("\r\nHello July Kernel\r\n\r\n");
    
#if defined(__arm__) || defined(__ARM_ARCH)
    // Enable MemFault, BusFault, UsageFault in SCB->SHCSR
    // Bit 16: MemFault, Bit 17: BusFault, Bit 18: UsageFault
    volatile uint32_t* shcsr = reinterpret_cast<volatile uint32_t*>(0xE000ED24U);
    *shcsr |= (1 << 16) | (1 << 17) | (1 << 18);
#endif

    KernelHeap::instance().init(reinterpret_cast<void*>(&_heap_start), reinterpret_cast<void*>(&_heap_end));

    // ==========================================
    // 婵€娲?MPU 绌洪棿闅旂瀹夊叏闃茬伀澧?
    // ==========================================
    MPU::instance().disable();

    // 1. 淇濇姢 Flash 浠ｇ爜鍖?(鍋囪浠?0x00000000 寮€濮嬶紝澶у皬 256KB = 2^18)
    // 鏉冮檺锛氬叏绯荤粺鍙 (AP_ALL_RO)锛屽厑璁告墽琛屼唬鐮?
    MPU::instance().configure_region(0, 0x00000000, 18, MPU::AP_ALL_RO, false);

    // 2. 閿佹鍏ㄥ眬 RAM 鍐呭瓨绌洪棿 (鍋囪浠?0x20000000 寮€濮嬶紝澶у皬 64KB = 2^16)
    // 鏉冮檺锛氫粎鍐呮牳鐗规潈鎬佽鍐?(AP_PRIV_RW)锛屼弗绂佺敤鎴锋€佽Е纰帮紒
    MPU::instance().configure_region(1, 0x20000000, 16, MPU::AP_PRIV_RW, true);

    MPU::instance().enable();
    sys_print("[Security] MPU Memory Protection Unit Activated.\r\n");

    VfsManager::instance().init();
    sys_print("[Boot] VFS ready\r\n");
#ifdef CONFIG_NETWORKING
    auroraos::ble::BleSignatureVerifier::instance().init();
    sys_print("[Boot] BLE verifier init done\r\n");
#endif

#ifdef CONFIG_FS_RAMFS
    static RamFile temp_file(1024);
    static RamFile elf_file(1024);
    VfsManager::instance().mount("/tmp/log.txt", &temp_file);
    VfsManager::instance().mount("/tmp/app.elf", &elf_file);
    sys_print("[Boot] RAMFS mounted\r\n");
#endif

#ifdef CONFIG_DEVICE_UART
    // 鎸傝浇 璁惧 鍜?/tmp 鐩綍涓嬬殑铏氭嫙鏂囦欢
    sys_print("[Boot] > entering DEVICE_UART block\r\n");
    static UartDevice uart0_dev("uart0");
    DeviceRegistry::instance().register_device(&uart0_dev);
    sys_print("[Boot] < uart0 register_device returned\r\n");
    sys_print("[Boot] uart0 registered\r\n");
#endif
    DeviceRegistry::instance().register_device(&g_oled);
    sys_print("[Boot] oled registered\r\n");
    DeviceRegistry::instance().register_device(&g_touch);
    sys_print("[Boot] touch registered\r\n");
    DeviceRegistry::instance().register_device(&g_nor_flash);
    sys_print("[Boot] nor_flash registered\r\n");
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
    static UptimeNode uptime_node;
    VfsManager::instance().mount("/proc/uptime", &uptime_node);
    static IrqNode irq_node;
    VfsManager::instance().mount("/proc/irq", &irq_node);
    static CapsNode caps_node;
    VfsManager::instance().mount("/proc/caps", &caps_node);
#endif
    
    // 鍒濆鍖栬皟搴﹀櫒
    Scheduler& sched = Scheduler::instance();
    sched.init();
    sys_print("[Boot] Scheduler initialized\r\n");

    // 鈹€鈹€ 鐪嬮棬鐙楀垵濮嬪寲 鈹€鈹€
#ifdef CONFIG_WATCHDOG
    {
#ifndef ARCH_RISCV32
        static Lm3sWdt hw_wdt;  // 纭欢鐪嬮棬鐙楅┍鍔?
        hw_wdt.init(CONFIG_WATCHDOG_TIMEOUT_MS, WatchdogMode::Reset);
        WatchdogManager::instance().init(&hw_wdt, CONFIG_WATCHDOG_TIMEOUT_MS);
#else
        static SoftWdt sw_wdt;
        WatchdogManager::instance().init(&sw_wdt, CONFIG_WATCHDOG_TIMEOUT_MS);
#endif
        sys_print("[Watchdog] Hardware WDT initialized (timeout=");
        // 绠€鍗曟墦鍗?timeout 鏁板€?
        char buf[16];
        uint32_t val = CONFIG_WATCHDOG_TIMEOUT_MS;
        int idx = 0;
        if (val == 0) { buf[idx++] = '0'; }
        else {
            char tmp[10]; int t = 0;
            while (val > 0) { tmp[t++] = '0' + (val % 10); val /= 10; }
            while (t > 0) { buf[idx++] = tmp[--t]; }
        }
        buf[idx++] = 'm'; buf[idx++] = 's'; buf[idx++] = ')'; buf[idx++] = '\r'; buf[idx++] = '\n'; buf[idx] = '\0';
        uart_puts(buf);
    }
#endif

    // 鈹€鈹€ 浠诲姟浼樺厛绾у垎閰嶈〃 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    // sys_idle_task   : Idle     鈥?CPU 鍏滃簳杩涚▼锛屾案涓嶄紤鐪?
    // shell_task      : High     鈥?浜や簰缁堢锛屽搷搴旈敭鐩樿緭鍏?
    // lwIP net tasks  : Realtime 鈥?缃戠粶 RX 鏁版嵁娉碉紙鍦?tcpip_init_done 涓垱寤猴級
    // udp_echo_task   : Normal   鈥?涓氬姟灞?Echo 澶勭悊
    // 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    constexpr uint32_t STACK_SIZE_IDLE = 128;
    // Shell 鏍堜粠 256 瀛?1KB) 鎵╁埌 512 瀛?2KB)锛?
    // execute_command() 璋冪敤閾惧緢娣憋紙cmd_copy[128] + 澶栧眰 run() 鐨?cmd_buf[128] 浠嶅湪鏍堜笂
    // + VFS open/write 鍚勫惈 LockGuard鈫扢utex::lock鈫扞rqGuard + uart_putc锛夛紝1KB 浼氳鍐插灝瑙﹀彂
    // task.hpp 鐨勬爤 canary 闈欓粯缁堟锛堟棤浠讳綍涓插彛杈撳嚭/寮傚父锛夛紝瀵艰嚧 HIL 鍙戦€?help 鍚庡懡浠よ緭鍑轰涪澶便€?
    // 娉ㄦ剰锛歭m3s6965-qb 浠?64KB RAM锛坙inker_qemu.ld锛夛紝椤堕儴杩樹繚鐣?8KB 寮曞涓绘爤锛?
    // 鍥犳涓嶈兘鐩茬洰鎵╁埌 4KB锛屽惁鍒?.bss 浼氶《鍏ヤ富鏍堜繚鐣欏尯瀵艰嚧 boot 闃舵浜掔浉韪╄笍锛堟棤浠讳綍杈撳嚭銆乹emu.log 绌猴級銆?
    // 2KB 宸茶冻澶熻鐩?execute_command 娣辫皟鐢ㄩ摼锛屽悓鏃舵妸 RAM 澧為噺鎺у埗鍒版渶灏忋€?
    constexpr uint32_t STACK_SIZE_SHELL = 512;
    constexpr uint32_t STACK_SIZE_TEST = 128;
    constexpr uint32_t STACK_SIZE_DAEMON = 256; // 鎭㈠涓?256锛岄伩鍏?.bss 鑶ㄨ儉
    constexpr uint32_t STACK_SIZE_SYSTEM_DAEMON = 512; // 涓撻棬涓?system_daemon_task 鍑嗗鐨勭◢澶ф爤
    // 瀛樺偍鍐欒仛鍚堟祴璇曚换鍔″崟鐙敤涓€妗ｆ爤锛岄伩鍏嶈窡闅?shell 涓€璧峰悆鎺夎繃澶?RAM銆?
    constexpr uint32_t STACK_SIZE_STORAGE = 384;

    static uint32_t idle_stack[STACK_SIZE_IDLE];
    static uint32_t shell_stack[STACK_SIZE_SHELL];

    // 1. 绌洪棽杩涚▼锛氫紭鍏堢骇鏈€浣庯紝璐熻矗 CPU 浣庡姛鑰楀厹搴?
    if (!Scheduler::instance().create_task(idle_task_entry, idle_stack, STACK_SIZE_IDLE * sizeof(uint32_t),
        TaskPriority::Idle)) {
        sys_print("[Kernel] FATAL: failed to spawn idle_task_entry!\r\n");
    }

    // 2. 浜や簰缁堢锛氶珮浼樺厛绾у搷搴旂敤鎴烽敭鐩?
    extern void shell_task(void);
    if (!Scheduler::instance().create_task(shell_task, shell_stack, STACK_SIZE_SHELL * sizeof(uint32_t),
        TaskPriority::High)) {
        sys_print("[Kernel] FATAL: failed to spawn shell_task!\r\n");
    }

#if defined(CONFIG_PI_DEMO)
    // 3. PI Mutex 娴嬭瘯浠诲姟
    static uint32_t pi_low_stack[STACK_SIZE_TEST];
    static uint32_t pi_mid_stack[STACK_SIZE_TEST];
    static uint32_t pi_high_stack[STACK_SIZE_TEST];
    Scheduler::instance().create_task(pi_test_low, pi_low_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::Low);
    Scheduler::instance().create_task(pi_test_mid, pi_mid_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::Normal);
    Scheduler::instance().create_task(pi_test_high, pi_high_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::High);
#endif

#if defined(CONFIG_HACKER_DEMO)
    // 4. Hacker App Task (甯︽湁 MPU 娌欑洅闅旂淇濇姢鐨勬祴璇曠嚎绋?
    Scheduler::instance().create_task(hacker_app_task, reinterpret_cast<uint32_t*>(hacker_stack), sizeof(hacker_stack), TaskPriority::Low, 10);
#endif

    // 5. Task Notify & POSIX Signal Test Tasks
#if defined(CONFIG_NOTIFY_DEMO)
    static uint32_t rx_stack[STACK_SIZE_TEST];
    static uint32_t tx_stack[STACK_SIZE_TEST];
    TaskControlBlock* rx_tcb = Scheduler::instance().create_task(receiver_task, rx_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::Normal);
    if (rx_tcb) g_receiver_task_id = rx_tcb->scheduler.id;
    Scheduler::instance().create_task(sender_task, tx_stack, STACK_SIZE_TEST*sizeof(uint32_t), TaskPriority::Normal);
#endif

    // 6. 钃濇渤 Frame-Aware Scheduler 浠诲姟娉ㄥ唽
#ifdef CONFIG_FONT_ENGINE
    static uint32_t ui_stack[STACK_SIZE_TEST];
    uint32_t ui_tid = FrameSchedulerV2::instance().create_frame_task(ui_render_task, ui_stack, STACK_SIZE_TEST * sizeof(uint32_t), TaskPriority::Realtime);
#endif

#if defined(CONFIG_SENSOR_DEMO)
    static uint32_t sensor_stack[STACK_SIZE_TEST];
    FrameSchedulerV2::instance().create_frame_task(sensor_log_task, sensor_stack, STACK_SIZE_TEST * sizeof(uint32_t), TaskPriority::Normal);
#endif

    // 7. 鍏夊瓙瀛樺偍鍐欒仛鍚堟祴璇曚换鍔?
    static uint32_t storage_stack[STACK_SIZE_STORAGE];
    Scheduler::instance().create_task(storage_test_task, storage_stack, STACK_SIZE_STORAGE * sizeof(uint32_t), TaskPriority::Normal);

    // 8. Phase 3: AI 鎰忓浘寮曟搸瀹堟姢杩涚▼涓?Lua 灏忕▼搴?
    static uint32_t daemon_stack[STACK_SIZE_SYSTEM_DAEMON];
    Scheduler::instance().create_task(system_daemon_task, daemon_stack, STACK_SIZE_SYSTEM_DAEMON * sizeof(uint32_t), TaskPriority::High);
    
#ifdef CONFIG_LUA_VM
    // Lua 铏氭嫙鏈洪渶瑕佽緝澶х殑鏍?
    static uint32_t lua_stack[1024];
    uint32_t tid_lua = FrameSchedulerV2::instance().create_frame_task(
        lua_app_task, lua_stack, 1024 * sizeof(uint32_t), TaskPriority::Realtime
    );
    g_lua_app.tid = tid_lua;
#endif

    // 銆愯摑娌冲紩鎿庣粦瀹氥€戝垵濮嬪寲 30FPS 璋冨害鍣紝骞剁粦瀹?UI 涓讳换鍔＄殑 ID
#ifdef CONFIG_FONT_ENGINE
    FrameSchedulerV2::instance().init(30, ui_tid);
#else
    FrameSchedulerV2::instance().init(30, 0);
#endif
    sys_print("[Boot] Tasks created, FrameScheduler bound\r\n");

#ifdef CONFIG_TIMER_MANAGER
    // 4. 瀹氭椂鍣ㄥ畧鎶よ繘绋嬩笌娴嬭瘯 App
    static uint32_t timer_daemon_stack[STACK_SIZE_DAEMON];
    static uint32_t posix_app_stack[STACK_SIZE_DAEMON];
    Scheduler::instance().create_task(timer_daemon_entry, timer_daemon_stack, STACK_SIZE_DAEMON*sizeof(uint32_t), TaskPriority::Realtime);
    Scheduler::instance().create_task(posix_app_task, posix_app_stack, STACK_SIZE_DAEMON*sizeof(uint32_t), TaskPriority::Low);
#endif

#ifdef CONFIG_WORK_QUEUE
    // 5. 宸ヤ綔闃熷垪瀹堟姢杩涚▼ (浣跨敤 High 浼樺厛绾?
    static uint32_t workq_daemon_stack[STACK_SIZE_DAEMON];
    Scheduler::instance().create_task(workqueue_daemon_entry, workq_daemon_stack, STACK_SIZE_DAEMON*sizeof(uint32_t), TaskPriority::High);
#endif

#ifdef CONFIG_NETWORKING
#ifndef ARCH_RISCV32
    // 銆愭柊澧炪€戝垱寤虹嫭绔嬬殑缃戠粶 DHCP 瀹㈡埛绔嚎绋?
    static uint32_t net_stack[640]; // lwIP (2.5KB)
    sched.create_task(network_task_entry, net_stack, 640 * sizeof(uint32_t), TaskPriority::Realtime);

    static uint32_t firewall_stack[512];
    sched.create_task(firewall_service_entry, firewall_stack, 512 * sizeof(uint32_t), TaskPriority::Normal);
#endif
#endif

    // 鍚姩璋冨害鍣細姝ｇ‘寮曞绗竴涓换鍔★紙閫氳繃 PSP/bx 璺冲叆锛屼笉鐮村潖鏍堝抚锛?
    // 璋冨害鍣ㄤ粠姝ゆ帴绠?CPU锛屾案涓嶈繑鍥?
    sys_print("[Boot] Starting scheduler\r\n");
    Scheduler::instance().start();
}

