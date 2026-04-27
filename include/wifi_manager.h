#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H
#include <stdbool.h>
#include "esp_err.h"
#include "app_types.h"
esp_err_t wifi_manager_start(const app_wifi_config_t *config);
esp_err_t wifi_manager_begin_provisioning_attempt(
    const char *device_id,
    const char *wifi_ssid,
    const char *wifi_password
);
void wifi_manager_get_provisioning_result(app_wifi_provision_result_t *result);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_ap_active(void);
#endif
