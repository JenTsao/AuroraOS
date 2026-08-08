// ============================================================
// wifi_monitor_task.cpp — WiFi Monitor Task (MPU-sandboxed)
//
// Creates a user-mode task that:
//   1. Runs in its own MPU sandbox region (TCB.mpu_sandbox)
//   2. Has a Capability granting USB peripheral register access
//   3. Polls the WiFi chipset for captured frames
//   4. Forwards frames to analysis modules via IPC
//   5. Handles channel hopping and capture loop
// ============================================================

#include "../kernel/task.hpp"
#include "../kernel/cspace.hpp"
#include "../kernel/mpu.hpp"
#include "../kernel/ipc.hpp"
#include "../net/wireless/wifi_monitor.hpp"
#include "../net/wireless/beacon_analyzer.hpp"
#include "../net/wireless/deauth_detector.hpp"
#include "../net/wireless/handshake_capture.hpp"
#include "../net/wireless/wireless_ids.hpp"
#include "../drivers/usb/usb_host.hpp"

using namespace auroraos::kernel;

// ---- IPC Message Types for WiFi Monitor ----

enum class WifiIpcMsgType : uint32_t {
    CapturedFrame    = 100,  // CapturedFrame payload
    ChannelCmd       = 101,  // channel change command
    MonitorCmd       = 102,  // monitor on/off command
    StatusResp       = 103,  // status response
};

struct WifiMonitorCommand {
    uint8_t  cmd;       // 0=set_channel, 1=start_monitor, 2=stop_monitor
    uint16_t param;     // channel freq or 0
    uint32_t reserved;
};

struct WifiMonitorStatus {
    bool     monitor_active;
    uint8_t  channel;
    uint32_t frames_captured;
    uint32_t uptime_ticks;
};

// ---- Driver selection ----
// Default to RTL8187L for LM3S (USB 2.0 capable); RTL8812AU
// needs USB 3.0 which LM3S6965 doesn't have.

static WifiMonitorDevice& get_wifi_device() {
    // Try RTL8187L first (USB 2.0, matches LM3S capability)
    Rtl8187lMonitor& rtl = Rtl8187lMonitor::instance();
    if (rtl.init()) return rtl;

    // Fallback: try RTL8812AU (will fail on LM3S USB 2.0, but OK
    // for future USB 3.0 targets)
    return Rtl8812auMonitor::instance();
}

// ---- Frame Forwarding ----
// Forwards captured frames to analysis modules.
// Called from the monitor task context.

static void forward_frame(const CapturedFrame& frame) {
    // Parse frame type
    int radiotap_len = frame.data[2] | (static_cast<int>(frame.data[3]) << 8);
    if (radiotap_len < 8) radiotap_len = 8;
    if (radiotap_len >= frame.frame_len) return;

    const uint8_t* mac_hdr = frame.data + radiotap_len;
    int mac_fc = mac_hdr[0] | (static_cast<int>(mac_hdr[1]) << 8);
    uint8_t type = (mac_fc >> 2) & 0x03;
    uint8_t subtype = (mac_fc >> 4) & 0x0F;

    // Route to appropriate analyzer
    if (type == 0) {
        // Management frame
        switch (subtype) {
            case 8:  // Beacon
                BeaconAnalyzer::instance().process_beacon(frame);
                break;
            case 12: // Deauth
            case 10: // Disassoc
                DeauthDetector::instance().process_deauth(frame);
                break;
            default:
                break;
        }
    } else if (type == 2) {
        // Data frame — may contain EAPOL
        HandshakeCapture::instance().process_eapol(frame);
    }
}

// ---- Channel Hopping (driven by ChannelHopper) ----

static void channel_hop_task(WifiMonitorDevice& dev, ChannelHopper& hopper) {
    if (hopper.channel_count() > 0) {
        uint16_t next_ch = hopper.hop();
        dev.set_channel(next_ch);

        // Sleep the dwell time (yield CPU)
        TaskControlBlock* current = Scheduler::instance().get_current_tcb();
        if (current) {
            Scheduler::instance().sleep_ms(hopper.dwell_ms());
        }
    }
}

// ---- WiFi Monitor Task Entry Point ----

// Static buffers (no heap allocation in user task)
static uint8_t g_wifi_task_stack[2048];
static uint8_t g_frame_rx_buffer[4096];  // large enough for USB 3.0 xfers

void wifi_monitor_task_entry() {
    WifiMonitorDevice& dev = get_wifi_device();

    if (dev.get_state() == WifiMonitorDevice::DriverState::Error) {
        // No WiFi device found — idle forever
        while (1) { Scheduler::instance().sleep_ms(1000); }
    }

    // Enter monitor mode
    dev.enter_monitor_mode();
    if (!dev.is_monitor_mode()) {
        while (1) { Scheduler::instance().sleep_ms(1000); }
    }

    // Configure channel hopper (2.4 GHz non-overlapping: 1, 6, 11)
    ChannelHopper hopper;
    uint16_t ch_list[16];
    int ch_count = 0;
    ChannelHopper::make_24ghz_non_overlap(ch_list, &ch_count);
    hopper.configure(ch_list, ch_count, 100);  // 100ms dwell

    // Load default IDS rules
    WirelessIds::instance().init();
    WirelessIds::instance().load_default_rules();

    // ---- Main Capture Loop ----
    while (1) {
        // CapturedFrame lives on the stack (MPU sandbox limits its scope)
        CapturedFrame frame;

        int ret = dev.capture_frame(frame);
        if (ret > 0) {
            forward_frame(frame);
        }

        // Periodic IDS scan (every ~100 frames)
        static int frame_counter = 0;
        if (++frame_counter >= 100) {
            frame_counter = 0;
            WirelessIds::instance().scan_beacons();
            WirelessIds::instance().scan_deauths();
        }

        // Channel hop
        channel_hop_task(dev, hopper);

        // Yield to scheduler
        TaskControlBlock* current = Scheduler::instance().get_current_tcb();
        if (current) {
            Scheduler::instance().sleep_ms(1);
        }
    }
}

// ---- Public API: create the sandboxed WiFi Monitor task ----

bool create_wifi_monitor_task() {
    Scheduler& sched = Scheduler::instance();

    // Create user-mode task with 2KB stack, 2^11 = 2048 byte MPU region
    TaskControlBlock* tcb = sched.create_task(
        wifi_monitor_task_entry,
        reinterpret_cast<uint32_t*>(g_wifi_task_stack),
        sizeof(g_wifi_task_stack),
        TaskPriority::Low,       // Background priority — don't block Shell
        11,                      // size_pow2: 2^11 = 2048 (stack size)
        TaskPrivilege::User      // User-mode — isolated by MPU
    );

    if (!tcb) return false;

    // Configure MPU sandbox: allow USB peripheral region (0x40050000-0x40050FFF)
    // This is the LM3S USB OTG register space.  The MPU will trap any
    // access outside the stack + USB peripheral range.
    tcb->mpu_sandbox.stack_base = reinterpret_cast<uintptr_t>(g_wifi_task_stack);
    tcb->mpu_sandbox.size_pow2  = 11;
    tcb->mpu_sandbox.version    = 1;
    tcb->mpu_sandbox.seal();

    // Grant USB host Capability (Cap ID = 0x10 = USB_PERIPHERAL)
    // The USB driver will validate this capability before accessing
    // the USB host controller registers.
    CSpace::cap_insert(tcb, 0x10, 0x40050000, 0x1000, 0x07); // R/W access to USB range

    return true;
}
