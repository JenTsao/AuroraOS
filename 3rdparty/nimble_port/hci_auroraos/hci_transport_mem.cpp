#include "nimble/nimble_port.h"
#include "os/os_mempool.h"
#include "os/os_mbuf.h"
#include "nimble/transport.h"
#include "nimble/transport_impl.h"
#include "net/ble/hci/hci_transport.hpp"
#include <string.h>

// HCI transport buffer type constants — these are AuroraOS-defined,
// not part of upstream NimBLE. They identify the buffer pool to allocate
// from in a vendor-neutral way.
#define BLE_HCI_TRANS_BUF_CMD     0
#define BLE_HCI_TRANS_BUF_EVT_HI  1
#define BLE_HCI_TRANS_BUF_EVT_LO  2

// =====================================================================
// NimBLE HCI Mempool (ACL Data and HCI Events)
// =====================================================================

#define HCI_CMD_BUF_SIZE    (260)
#define HCI_CMD_BUF_COUNT   (2)

#define HCI_EVT_BUF_SIZE    (260)
#define HCI_EVT_BUF_COUNT   (4)

#define ACL_BUF_SIZE        (260) // BLE 4.2+ Data Length Extension might need more
#define ACL_BUF_COUNT       (4)

extern "C" {

static struct os_mempool hci_cmd_pool;
static os_membuf_t hci_cmd_buf[OS_MEMPOOL_SIZE(HCI_CMD_BUF_COUNT, HCI_CMD_BUF_SIZE)];

static struct os_mempool hci_evt_pool;
static os_membuf_t hci_evt_buf[OS_MEMPOOL_SIZE(HCI_EVT_BUF_COUNT, HCI_EVT_BUF_SIZE)];

static struct os_mempool_ext acl_pool;
static os_membuf_t acl_buf[OS_MEMPOOL_SIZE(ACL_BUF_COUNT, ACL_BUF_SIZE)];
static struct os_mbuf_pool acl_mbuf_pool;

// 初始化 HCI 内存池 (在 ble_hs_init 之前由 NimBLE Porting 初始化层调用)
void aurora_ble_hci_mempool_init(void) {
    os_mempool_init(&hci_cmd_pool, HCI_CMD_BUF_COUNT, HCI_CMD_BUF_SIZE, hci_cmd_buf, "hci_cmd_pool");
    os_mempool_init(&hci_evt_pool, HCI_EVT_BUF_COUNT, HCI_EVT_BUF_SIZE, hci_evt_buf, "hci_evt_pool");
    os_mempool_ext_init(&acl_pool, ACL_BUF_COUNT, ACL_BUF_SIZE, acl_buf, "acl_pool");
    os_mbuf_pool_init(&acl_mbuf_pool, &acl_pool.mpe_mp, ACL_BUF_SIZE, ACL_BUF_COUNT);
}

// ---------------------------------------------------------------------
// NimBLE Transport Implementation (Host <-> Controller)
// ---------------------------------------------------------------------
void ble_transport_ll_init(void) {
    aurora_ble_hci_mempool_init();
    if (auroraos::ble::hci::g_hci_transport) {
        auroraos::ble::hci::g_hci_transport->init();
    }
}

void *ble_transport_alloc_cmd(void) {
    return os_memblock_get(&hci_cmd_pool);
}

void *ble_transport_alloc_evt(int discardable) {
    (void)discardable;
    return os_memblock_get(&hci_evt_pool);
}

struct os_mbuf *ble_transport_alloc_acl_from_hs(void) {
    return os_mbuf_get_pkthdr(&acl_mbuf_pool, 0);
}

struct os_mbuf *ble_transport_alloc_acl_from_ll(void) {
    return os_mbuf_get_pkthdr(&acl_mbuf_pool, 0);
}

struct os_mbuf *ble_transport_alloc_iso_from_hs(void) {
    return nullptr;
}

struct os_mbuf *ble_transport_alloc_iso_from_ll(void) {
    return nullptr;
}

void ble_transport_free(void *buf) {
    if (os_memblock_from(&hci_cmd_pool, buf)) {
        os_memblock_put(&hci_cmd_pool, buf);
    } else if (os_memblock_from(&hci_evt_pool, buf)) {
        os_memblock_put(&hci_evt_pool, buf);
    }
}

int ble_transport_to_ll_cmd_impl(void *buf) {
    if (!buf) return -1;
    const uint8_t *cmd = static_cast<const uint8_t *>(buf);
    size_t len = 3 + cmd[2]; // [Opcode: 2B] [ParamLen: 1B] [Params: NB]
    int rc = 0;
    if (auroraos::ble::hci::g_hci_transport) {
        rc = auroraos::ble::hci::g_hci_transport->send_cmd(cmd, len);
    }
    ble_transport_free(buf);
    return rc;
}

int ble_transport_to_ll_acl_impl(struct os_mbuf *om) {
    if (!om) return -1;
    uint16_t len = OS_MBUF_PKTLEN(om);
    uint8_t buf[260];
    if (len > sizeof(buf)) {
        os_mbuf_free_chain(om);
        return -1;
    }
    os_mbuf_copydata(om, 0, len, buf);
    int rc = 0;
    if (auroraos::ble::hci::g_hci_transport) {
        rc = auroraos::ble::hci::g_hci_transport->send_acl(buf, len);
    }
    os_mbuf_free_chain(om);
    return rc;
}

int ble_transport_to_ll_iso_impl(struct os_mbuf *om) {
    if (om) os_mbuf_free_chain(om);
    return 0;
}

// ---------------------------------------------------------------------
// Controller -> Host RX Bridge Hooks
// ---------------------------------------------------------------------
void aurora_nimble_rx_event(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    void *evt_buf = ble_transport_alloc_evt(0);
    if (evt_buf) {
        memcpy(evt_buf, data, len);
        ble_transport_to_hs_evt(evt_buf);
    }
}

void aurora_nimble_rx_acl(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    struct os_mbuf *om = ble_transport_alloc_acl_from_ll();
    if (om) {
        os_mbuf_append(om, data, static_cast<uint16_t>(len));
        ble_transport_to_hs_acl(om);
    }
}

// ---------------------------------------------------------------------
// Legacy Compatibility Buffer Allocation API
// ---------------------------------------------------------------------
uint8_t *ble_hci_trans_buf_alloc(int type) {
    switch (type) {
        case BLE_HCI_TRANS_BUF_CMD:
            return (uint8_t *)ble_transport_alloc_cmd();
        case BLE_HCI_TRANS_BUF_EVT_HI:
        case BLE_HCI_TRANS_BUF_EVT_LO:
            return (uint8_t *)ble_transport_alloc_evt(0);
        default:
            return nullptr;
    }
}

void ble_hci_trans_buf_free(uint8_t *buf) {
    ble_transport_free(buf);
}

} // extern "C"

