#ifndef SCHEDULE_MANAGER_H
#define SCHEDULE_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

#include "app_types.h"

esp_err_t schedule_manager_init(void);
esp_err_t schedule_manager_restore(void);
esp_err_t schedule_manager_handle_mqtt_payload(
    const char *payload,
    char *error_code,
    size_t error_code_len,
    char *error_message,
    size_t error_message_len
);
esp_err_t schedule_manager_build_payload(const char *device_id, char **out_json);
esp_err_t schedule_manager_get_store(app_schedule_store_t *store);
bool schedule_manager_has_schedules(void);

#endif
