#include "mqtt_app.h"

#include "esp_log.h"

#include "app_config.h"
#include "comm_storage.h"
#include "local_server.h"
#include "log_tags.h"
#include "mqtt_manager.h"
#include "wifi_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool s_started;

static void mqtt_publish_task(void *arg) {
    (void)arg;

    while (1) {
        if (mqtt_manager_is_connected()) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_state());
        }

        vTaskDelay(pdMS_TO_TICKS(APP_STATUS_PUBLISH_INTERVAL_MS));
    }
}

esp_err_t mqtt_app_start(void) {
    app_wifi_config_t wifi = {0};
    app_mqtt_config_t mqtt = {0};

    if (s_started) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(comm_storage_load_wifi_config(&wifi));
    ESP_ERROR_CHECK_WITHOUT_ABORT(comm_storage_load_mqtt_config(&mqtt));
    ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_configure(&mqtt));

    ESP_LOGI(
        LOG_TAG_MQTT_MANAGER,
        "Comunicacao preparada: deviceId=%s topicPrefix=%s broker=%s",
        mqtt.device_id,
        mqtt.topic_prefix,
        mqtt.broker_uri
    );

    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi_manager_start(&wifi));
    ESP_ERROR_CHECK_WITHOUT_ABORT(local_server_start());

    xTaskCreate(mqtt_publish_task, "mqtt_pub", APP_MQTT_TASK_STACK_SIZE, NULL, APP_MQTT_TASK_PRIORITY, NULL);
    s_started = true;

    return ESP_OK;
}
