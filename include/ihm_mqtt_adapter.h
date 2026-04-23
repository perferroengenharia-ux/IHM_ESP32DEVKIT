#ifndef IHM_MQTT_ADAPTER_H
#define IHM_MQTT_ADAPTER_H
#include "esp_err.h"
#include "app_types.h"
esp_err_t ihm_mqtt_adapter_get_state(device_state_t *state);
esp_err_t ihm_mqtt_adapter_get_capabilities(device_capabilities_t *capabilities);
esp_err_t ihm_mqtt_adapter_execute_command(const app_command_t *command, command_result_t *result);
#endif
