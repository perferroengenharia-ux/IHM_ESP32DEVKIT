#ifndef COMM_STORAGE_H
#define COMM_STORAGE_H
#include "esp_err.h"
#include "app_types.h"
esp_err_t comm_storage_load_wifi_config(app_wifi_config_t *config);
esp_err_t comm_storage_load_mqtt_config(app_mqtt_config_t *config);
esp_err_t comm_storage_save_provisioning(
    const char *device_id,
    const char *wifi_ssid,
    const char *wifi_password
);
#endif
