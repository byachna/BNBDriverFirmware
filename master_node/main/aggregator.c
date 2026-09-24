#include <string.h>
#include "aggregator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static wd_ap_record_t s_aps[WD_AGG_MAX_APS];
static uint32_t s_count = 0;
static SemaphoreHandle_t s_lock;
static wd_agg_update_cb_t s_cb;

/* Creates the mutex and resets the accumulated access-point table. */
void wd_aggregator_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_count = 0;
}

/* Registers the callback notified after an access point is added or updated. */
void wd_aggregator_set_update_cb(wd_agg_update_cb_t cb)
{
    s_cb = cb;
}

/* Finds a record by source radio and address and returns its table index, or -1. */
static int find_by_address(wd_record_type_t record_type, const uint8_t *address)
{
    for (uint32_t i = 0; i < s_count; i++) {
        if (s_aps[i].record_type == record_type &&
            memcmp(s_aps[i].bssid, address, WD_MAC_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Adds or replaces a record while protecting the table, then notifies callers. */
void wd_aggregator_add(const wd_ap_record_t *rec)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    bool is_new = false;
    int idx = find_by_address((wd_record_type_t)rec->record_type, rec->bssid);
    if (idx < 0) {
        if (s_count < WD_AGG_MAX_APS) {
            idx = (int)s_count++;
            is_new = true;
        } else {
            /* Table full: overwrite the oldest slot (index 0) to keep going. */
            idx = 0;
        }
    }
    s_aps[idx] = *rec;

    xSemaphoreGive(s_lock);

    if (s_cb) {
        s_cb(rec, is_new);
    }
}

/* Returns the number of access-point records currently stored. */
uint32_t wd_aggregator_count(void)
{
    return s_count;
}

/* Copies up to max_out records into out and returns the number copied. */
uint32_t wd_aggregator_snapshot(wd_ap_record_t *out, uint32_t max_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t n = (s_count < max_out) ? s_count : max_out;
    memcpy(out, s_aps, n * sizeof(wd_ap_record_t));
    xSemaphoreGive(s_lock);
    return n;
}

/* Removes all accumulated access-point records. */
void wd_aggregator_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    xSemaphoreGive(s_lock);
}
