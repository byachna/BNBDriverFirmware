#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "ble_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"

static const char *TAG = "wd_ble";

/* 6e400001-b5a3-f393-e0a9-e50e24dcca9e style custom 128-bit UUIDs. */
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
static const ble_uuid128_t s_chr_ap_stream_uuid =
    BLE_UUID128_INIT(0x02, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
static const ble_uuid128_t s_chr_control_uuid =
    BLE_UUID128_INIT(0x03, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
static const ble_uuid128_t s_chr_status_uuid =
    BLE_UUID128_INIT(0x04, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
                      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);

typedef struct __attribute__((packed)) {
    uint32_t ap_count;
    uint32_t node_count;
} wd_ble_status_t;

static uint16_t s_ap_stream_val_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_subscribed = false;
static uint8_t s_own_addr_type;

static wd_ble_control_cb_t s_control_cb;
static wd_ble_count_fn_t s_ap_count_fn;
static wd_ble_count_fn_t s_node_count_fn;

static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* Configures the legacy advertisement and restarts advertising indefinitely. */
static void start_advertising(void)
{
    /* Keep the primary advertising packet under the 31-byte legacy limit:
     * flags + tx power + name fits, but adding the 128-bit service UUID on
     * top does not, which made ble_gap_adv_set_fields() silently fail and
     * advertising never started. The UUID goes in the scan response instead. */
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.uuids128 = (ble_uuid128_t[]) { s_svc_uuid };
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields failed rc=%d", rc);
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed rc=%d", rc);
    }
}

/* Tracks BLE connection, subscription, MTU, and advertising lifecycle events. */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect status=%d", event->connect.status);
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
        } else {
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ap_stream_val_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "phone %s AP stream notifications", s_subscribed ? "subscribed to" : "unsubscribed from");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update conn=%d mtu=%d", event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* Serves BLE control writes, status reads, and AP stream reads. */
static int chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    if (ble_uuid_cmp(uuid, &s_chr_control_uuid.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        uint8_t cmd = 0;
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, &cmd, sizeof(cmd), &out_len);
        if (rc != 0 || out_len < 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (s_control_cb) {
            s_control_cb((wd_ble_cmd_t)cmd);
        }
        return 0;
    }

    if (ble_uuid_cmp(uuid, &s_chr_status_uuid.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        wd_ble_status_t status = {
            .ap_count = s_ap_count_fn ? s_ap_count_fn() : 0,
            .node_count = s_node_count_fn ? s_node_count_fn() : 0,
        };
        int rc = os_mbuf_append(ctxt->om, &status, sizeof(status));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ble_uuid_cmp(uuid, &s_chr_ap_stream_uuid.u) == 0) {
        /* Read is allowed but the interesting data arrives via notify. */
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            uint8_t empty = 0;
            os_mbuf_append(ctxt->om, &empty, sizeof(empty));
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_chr_ap_stream_uuid.u,
                .access_cb = chr_access_cb,
                .val_handle = &s_ap_stream_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_chr_control_uuid.u,
                .access_cb = chr_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_chr_status_uuid.u,
                .access_cb = chr_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    { 0 },
};

/* Receives GATT registration events; handles are retained through definitions. */
static void gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)ctxt;
    (void)arg;
}

/* Logs a NimBLE host reset so the reset reason is visible in diagnostics. */
static void on_reset(int reason)
{
    ESP_LOGE(TAG, "nimble host reset, reason=%d", reason);
}

/* Resolves the local BLE address type and begins advertising after sync. */
static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer address type; rc=%d", rc);
        return;
    }
    start_advertising();
}

/* Runs the NimBLE host event loop on its FreeRTOS task. */
static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Installs the callback for commands received from the BLE control characteristic. */
void wd_ble_set_control_cb(wd_ble_control_cb_t cb)
{
    s_control_cb = cb;
}

/* Installs providers used to build the on-demand BLE status response. */
void wd_ble_set_status_providers(wd_ble_count_fn_t ap_count_fn, wd_ble_count_fn_t node_count_fn)
{
    s_ap_count_fn = ap_count_fn;
    s_node_count_fn = node_count_fn;
}

/* Initializes NimBLE, registers the GATT service, and starts its host task. */
void wd_ble_init(void)
{
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_register_cb;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    assert(rc == 0);

    ble_svc_gap_device_name_set(CONFIG_WD_BLE_DEVICE_NAME);

    nimble_port_freertos_init(host_task);
}

/* Notifies a subscribed BLE client with one discovered AP record. */
void wd_ble_notify_ap(const wd_ap_record_t *rec)
{
    if (!s_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(rec, sizeof(*rec));
    if (!om) {
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_ap_stream_val_handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "notify failed rc=%d", rc);
    }
}
