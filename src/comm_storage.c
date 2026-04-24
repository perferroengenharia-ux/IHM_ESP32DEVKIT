#include "comm_storage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

#include "log_tags.h"

static void copy_string(char *destination, size_t destination_len, const char *source) {
    if (destination == NULL || destination_len == 0) {
        return;
    }

    destination[0] = '\0';

    if (source != NULL) {
        strncpy(destination, source, destination_len - 1);
    }
}

static void fill_default_device_id(char *device_id, size_t device_id_len) {
    uint8_t mac[6] = {0};

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, device_id_len, "ihm32-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void nvs_get_string_or_keep(
    nvs_handle_t handle,
    const char *key,
    char *value,
    size_t value_len
) {
    size_t required = value_len;
    esp_err_t err = nvs_get_str(handle, key, value, &required);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(LOG_TAG_COMM_STORAGE, "Falha ao ler %s: %s", key, esp_err_to_name(err));
    }
}

esp_err_t comm_storage_load_wifi_config(app_wifi_config_t *config) {
    nvs_handle_t handle = 0;
    uint8_t enabled = APP_WIFI_STA_SSID_DEFAULT[0] != '\0';

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->sta_enabled = enabled != 0;
    config->max_retry = APP_WIFI_MAXIMUM_RETRY;
    copy_string(config->sta_ssid, sizeof(config->sta_ssid), APP_WIFI_STA_SSID_DEFAULT);
    copy_string(config->sta_password, sizeof(config->sta_password), APP_WIFI_STA_PASSWORD_DEFAULT);

    if (nvs_open(APP_NVS_NAMESPACE_COMM, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_OK;
    }

    (void)nvs_get_u8(handle, "wifi_en", &enabled);
    config->sta_enabled = enabled != 0;
    nvs_get_string_or_keep(handle, "wifi_ssid", config->sta_ssid, sizeof(config->sta_ssid));
    nvs_get_string_or_keep(handle, "wifi_pass", config->sta_password, sizeof(config->sta_password));
    nvs_close(handle);

    return ESP_OK;
}

esp_err_t comm_storage_load_mqtt_config(app_mqtt_config_t *config) {
    nvs_handle_t handle = 0;
    uint8_t enabled = APP_MQTT_ENABLED_DEFAULT;
    uint8_t tls = APP_MQTT_USE_TLS_DEFAULT;
    uint16_t port = APP_MQTT_PORT_DEFAULT;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->enabled = APP_MQTT_ENABLED_DEFAULT != 0;
    config->use_tls = APP_MQTT_USE_TLS_DEFAULT != 0;
    config->port = APP_MQTT_PORT_DEFAULT;
    config->keepalive_sec = APP_MQTT_KEEPALIVE_SEC_DEFAULT;

    copy_string(config->broker_uri, sizeof(config->broker_uri), APP_MQTT_URI_DEFAULT);
    copy_string(config->username, sizeof(config->username), APP_MQTT_USERNAME_DEFAULT);
    copy_string(config->password, sizeof(config->password), APP_MQTT_PASSWORD_DEFAULT);
    copy_string(config->topic_prefix, sizeof(config->topic_prefix), APP_TOPIC_PREFIX_DEFAULT);
    fill_default_device_id(config->device_id, sizeof(config->device_id));

    if (nvs_open(APP_NVS_NAMESPACE_COMM, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_OK;
    }

    (void)nvs_get_u8(handle, "mqtt_en", &enabled);
    (void)nvs_get_u8(handle, "mqtt_tls", &tls);
    (void)nvs_get_u16(handle, "mqtt_port", &port);

    config->enabled = enabled != 0;
    config->use_tls = tls != 0;
    config->port = port;

    nvs_get_string_or_keep(handle, "mqtt_uri", config->broker_uri, sizeof(config->broker_uri));
    nvs_get_string_or_keep(handle, "mqtt_user", config->username, sizeof(config->username));
    nvs_get_string_or_keep(handle, "mqtt_pass", config->password, sizeof(config->password));
    nvs_get_string_or_keep(handle, "topic", config->topic_prefix, sizeof(config->topic_prefix));
    nvs_get_string_or_keep(handle, "device_id", config->device_id, sizeof(config->device_id));
    nvs_close(handle);

    if (config->topic_prefix[0] == '\0') {
        copy_string(config->topic_prefix, sizeof(config->topic_prefix), APP_TOPIC_PREFIX_DEFAULT);
    }

    if (config->device_id[0] == '\0') {
        fill_default_device_id(config->device_id, sizeof(config->device_id));
    }

    return ESP_OK;
}

esp_err_t comm_storage_save_provisioning(
    const char *device_id,
    const char *wifi_ssid,
    const char *wifi_password
) {
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;

    if (
        device_id == NULL || device_id[0] == '\0' ||
        wifi_ssid == NULL || wifi_ssid[0] == '\0' ||
        strlen(device_id) > APP_DEVICE_ID_MAX_LEN ||
        strlen(wifi_ssid) > APP_SSID_MAX_LEN ||
        (wifi_password != NULL && strlen(wifi_password) > APP_WIFI_PASSWORD_MAX_LEN)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(APP_NVS_NAMESPACE_COMM, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, "wifi_en", 1);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "wifi_ssid", wifi_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "wifi_pass", wifi_password != NULL ? wifi_password : "");
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "device_id", device_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(
            LOG_TAG_COMM_STORAGE,
            "Provisionamento salvo: deviceId=%s wifiSsid=%s",
            device_id,
            wifi_ssid
        );
    }

    return err;
}
