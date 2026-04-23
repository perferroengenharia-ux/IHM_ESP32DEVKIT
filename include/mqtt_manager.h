#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H
#include <stdbool.h>
#include "esp_err.h"
#include "app_types.h"
esp_err_t mqtt_manager_configure(const app_mqtt_config_t *config);
esp_err_t mqtt_manager_start(void);
bool mqtt_manager_is_connected(void);
esp_err_t mqtt_manager_publish_full_snapshot(void);
esp_err_t mqtt_manager_publish_state(void);
esp_err_t mqtt_manager_publish_capabilities(void);
esp_err_t mqtt_manager_publish_schedules(void);
#endif
