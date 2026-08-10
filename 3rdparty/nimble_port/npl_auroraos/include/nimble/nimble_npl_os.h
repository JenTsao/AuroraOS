#ifndef _NIMBLE_NPL_OS_H_
#define _NIMBLE_NPL_OS_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// AuroraOS NPL (NimBLE Porting Layer) Abstractions

#define BLE_NPL_TIME_FOREVER    (0xFFFFFFFF)

typedef uint32_t ble_npl_time_t;
typedef int32_t ble_npl_stime_t;

// Forward declare AuroraOS primitive wrappers
struct ble_npl_event {
    bool queued;
    void (*fn)(struct ble_npl_event *ev);
    void *arg;
};

struct ble_npl_eventq {
    void *q; // Pointer to AuroraOS MessageQueue<struct ble_npl_event*, N>
};

struct ble_npl_callout {
    void *handle; // Pointer to AuroraOS Timer
    struct ble_npl_eventq *evq;
    struct ble_npl_event ev;
};

struct ble_npl_mutex {
    void *mu; // Pointer to AuroraOS Mutex
};

struct ble_npl_sem {
    void *sem; // Pointer to AuroraOS Semaphore
};

// ========================================================
// HCI Transport Interfaces (To be implemented by our driver)
// ========================================================
int ble_hci_trans_ll_evt_tx(uint8_t *hci_ev);
int ble_hci_trans_ll_acl_tx(void *om);

#ifdef __cplusplus
}
#endif

#endif // _NIMBLE_NPL_OS_H_
