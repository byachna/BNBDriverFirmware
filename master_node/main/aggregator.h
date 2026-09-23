#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "wardrive_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WD_AGG_MAX_APS 512

/* Notified once per newly-seen or updated AP record (already deduplicated
 * by BSSID). Called from whichever task received the record; keep it fast. */
typedef void (*wd_agg_update_cb_t)(const wd_ap_record_t *rec, bool is_new);

void wd_aggregator_init(void);
void wd_aggregator_set_update_cb(wd_agg_update_cb_t cb);

/* Insert or refresh an AP record. Thread-safe. */
void wd_aggregator_add(const wd_ap_record_t *rec);

uint32_t wd_aggregator_count(void);

/* Copy up to max_out records into out. Returns number copied. Thread-safe. */
uint32_t wd_aggregator_snapshot(wd_ap_record_t *out, uint32_t max_out);

void wd_aggregator_clear(void);

#ifdef __cplusplus
}
#endif
