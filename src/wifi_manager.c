#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "comm_storage.h"
#include "log_tags.h"
#include "mqtt_manager.h"
#include "time_sync.h"

static bool s_initialized;
static bool s_sta_connected;
static bool s_ap_active;
static int s_retry_count;
static app_wifi_config_t s_config;

static void build_ap_ssid(char *ssid, size_t ssid_len, const char *device_id) {
    const char *suffix = device_id;
    size_t device_len = device_id != NULL ? strlen(device_id) : 0;

    if (ssid == NULL || ssid_len == 0) {
        return;
    }

    if (device_len > 6) {
        suffix = device_id + (device_len - 6);
    }

    snprintf(ssid, ssid_len, "%s%s", APP_WIFI_AP_SSID_PREFIX, suffix != NULL ? suffix : "SETUP");
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)base;
    (void)event_data;

    if (event_id == WIFI_EVENT_STA_START) {
        if (s_config.sta_enabled && s_config.sta_ssid[0] != '\0') {
            ESP_LOGI(LOG_TAG_WIFI_MANAGER, "Wi-Fi STA iniciado");
            esp_wifi_connect();
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;

        if (s_config.sta_enabled && s_config.sta_ssid[0] != '\0' && s_retry_count < s_config.max_retry) {
            s_retry_count++;
            ESP_LOGW(
                LOG_TAG_WIFI_MANAGER,
                "Wi-Fi desconectado, reconectando (%d/%d)",
                s_retry_count,
                s_config.max_retry
            );
            esp_wifi_connect();
        } else if (s_config.sta_enabled && s_config.sta_ssid[0] != '\0') {
            ESP_LOGE(
                LOG_TAG_WIFI_MANAGER,
                "Wi-Fi STA indisponivel apos limite de tentativas; fallback local continua ativo"
            );
        }
    } else if (event_id == WIFI_EVENT_AP_START) {
        s_ap_active = true;
        ESP_LOGI(LOG_TAG_WIFI_MANAGER, "Wi-Fi AP de provisionamento/fallback ativo");
    } else if (event_id == WIFI_EVENT_AP_STOP) {
        s_ap_active = false;
        ESP_LOGW(LOG_TAG_WIFI_MANAGER, "Wi-Fi AP local interrompido");
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)base;
    (void)event_data;

    if (event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        s_sta_connected = true;
        ESP_LOGI(LOG_TAG_WIFI_MANAGER, "Wi-Fi conectado, iniciando SNTP e MQTT");
        time_sync_notify_network_ready();
        ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_start());
    }
}

esp_err_t wifi_manager_start(const app_wifi_config_t *config) {
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t sta_config = {0};
    wifi_config_t ap_config = {0};
    app_mqtt_config_t mqtt_config = {0};
    wifi_mode_t mode = WIFI_MODE_AP;
    char ap_ssid[APP_SSID_MAX_LEN + 1] = {0};
    esp_err_t loop_err = ESP_OK;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(s_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(comm_storage_load_mqtt_config(&mqtt_config));
    build_ap_ssid(ap_ssid, sizeof(ap_ssid), mqtt_config.device_id);

    if (!s_initialized) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
        loop_err = esp_event_loop_create_default();
        if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
            return loop_err;
        }

        esp_netif_create_default_wifi_sta();
        esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
        ESP_ERROR_CHECK(
            esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL)
        );
        ESP_ERROR_CHECK(
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, NULL)
        );
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        s_initialized = true;
    }

    if (s_config.sta_enabled && s_config.sta_ssid[0] != '\0') {
        mode = WIFI_MODE_APSTA;
        strncpy((char *)sta_config.sta.ssid, s_config.sta_ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, s_config.sta_password, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.failure_retry_cnt = s_config.max_retry;
        sta_config.sta.threshold.authmode =
            s_config.sta_password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_config.sta.pmf_cfg.capable = true;
        sta_config.sta.pmf_cfg.required = false;
    }

    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    strncpy(
        (char *)ap_config.ap.password,
        APP_WIFI_AP_PASSWORD_DEFAULT,
        sizeof(ap_config.ap.password) - 1
    );
    ap_config.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    ap_config.ap.channel = APP_WIFI_AP_CHANNEL;
    ap_config.ap.max_connection = APP_WIFI_AP_MAX_CONNECTIONS;
    ap_config.ap.authmode =
        APP_WIFI_AP_PASSWORD_DEFAULT[0] != '\0' ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_LOGI(
        LOG_TAG_WIFI_MANAGER,
        "Inicializando Wi-Fi: mode=%s AP=%s STA=%s",
        mode == WIFI_MODE_APSTA ? "APSTA" : "AP",
        ap_ssid,
        s_config.sta_ssid[0] != '\0' ? s_config.sta_ssid : "(nao configurado)"
    );

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    if (mode == WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

bool wifi_manager_is_connected(void) {
    return s_sta_connected;
}

bool wifi_manager_is_ap_active(void) {
    return s_ap_active;
}
