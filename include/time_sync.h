#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

esp_err_t time_sync_init(void);
void time_sync_notify_network_ready(void);
bool time_sync_is_time_valid(void);
esp_err_t time_sync_set_utc_time_from_iso8601(const char *timestamp);
void time_sync_set_offset_minutes(int offset_minutes);
int time_sync_get_offset_minutes(void);
bool time_sync_get_local_time(struct tm *out_tm, time_t *out_utc_now);

#endif
