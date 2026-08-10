#include "nimble/nimble_port.h"
#include "os/os_mempool.h"

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

static struct os_mempool acl_pool;
static os_membuf_t acl_buf[OS_MEMPOOL_SIZE(ACL_BUF_COUNT, ACL_BUF_SIZE)];

// 初始化 HCI 内存池 (在 ble_hs_init 之前由 NimBLE Porting 初始化层调用)
void aurora_ble_hci_mempool_init(void) {
    os_mempool_init(&hci_cmd_pool, HCI_CMD_BUF_COUNT, HCI_CMD_BUF_SIZE, hci_cmd_buf, "hci_cmd_pool");
    os_mempool_init(&hci_evt_pool, HCI_EVT_BUF_COUNT, HCI_EVT_BUF_SIZE, hci_evt_buf, "hci_evt_pool");
    os_mempool_init(&acl_pool, ACL_BUF_COUNT, ACL_BUF_SIZE, acl_buf, "acl_pool");
}

// ---------------------------------------------------------------------
// HCI Transport Buffer Allocation API
// ---------------------------------------------------------------------
uint8_t *ble_hci_trans_buf_alloc(int type) {
    switch (type) {
        case BLE_HCI_TRANS_BUF_CMD:
            return (uint8_t *)os_memblock_get(&hci_cmd_pool);
        case BLE_HCI_TRANS_BUF_EVT_HI:
        case BLE_HCI_TRANS_BUF_EVT_LO:
            return (uint8_t *)os_memblock_get(&hci_evt_pool);
        default:
            return nullptr;
    }
}

void ble_hci_trans_buf_free(uint8_t *buf) {
    if (os_memblock_from(&hci_cmd_pool, buf)) {
        os_memblock_put(&hci_cmd_pool, buf);
    } else if (os_memblock_from(&hci_evt_pool, buf)) {
        os_memblock_put(&hci_evt_pool, buf);
    }
}

} // extern "C"
