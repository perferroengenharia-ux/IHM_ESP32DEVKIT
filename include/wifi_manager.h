#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H
#include <stdbool.h>
#include "esp_err.h"
#include "app_types.h"
esp_err_t wifi_manager_start(const app_wifi_config_t *config);
bool wifi_manager_is_connected(void);
#endif
