#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
static uint32_t s_provision_generation;
static app_wifi_config_t s_config;
static app_wifi_provision_result_t s_provision_result;
static char s_pending_device_id[APP_DEVICE_ID_MAX_LEN + 1] = {0};
static char s_pending_wifi_ssid[APP_SSID_MAX_LEN + 1] = {0};
static char s_pending_wifi_password[APP_WIFI_PASSWORD_MAX_LEN + 1] = {0};

static void copy_string(char *destination, size_t destination_len, const char *source) {
    if (destination == NULL || destination_len == 0) {
        return;
    }

    destination[0] = '\0';

    if (source != NULL) {
        strncpy(destination, source, destination_len - 1);
    }
}

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

static void fill_sta_config(wifi_config_t *sta_config, const app_wifi_config_t *config) {
    if (sta_config == NULL || config == NULL) {
        return;
    }

    memset(sta_config, 0, sizeof(*sta_config));
    strncpy((char *)sta_config->sta.ssid, config->sta_ssid, sizeof(sta_config->sta.ssid) - 1);
    strncpy((char *)sta_config->sta.password, config->sta_password, sizeof(sta_config->sta.password) - 1);
    sta_config->sta.failure_retry_cnt = config->max_retry;
    sta_config->sta.threshold.authmode =
        config->sta_password[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta_config->sta.pmf_cfg.capable = true;
    sta_config->sta.pmf_cfg.required = false;
}

static void fill_ap_config(wifi_config_t *ap_config) {
    app_mqtt_config_t mqtt_config = {0};
    char ap_ssid[APP_SSID_MAX_LEN + 1] = {0};

    if (ap_config == NULL) {
        return;
    }

    memset(ap_config, 0, sizeof(*ap_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(comm_storage_load_mqtt_config(&mqtt_config));
    build_ap_ssid(ap_ssid, sizeof(ap_ssid), mqtt_config.device_id);

    strncpy((char *)ap_config->ap.ssid, ap_ssid, sizeof(ap_config->ap.ssid) - 1);
    strncpy((char *)ap_config->ap.password, APP_WIFI_AP_PASSWORD_DEFAULT, sizeof(ap_config->ap.password) - 1);
    ap_config->ap.ssid_len = (uint8_t)strlen(ap_ssid);
    ap_config->ap.channel = APP_WIFI_AP_CHANNEL;
    ap_config->ap.max_connection = APP_WIFI_AP_MAX_CONNECTIONS;
    ap_config->ap.authmode =
        APP_WIFI_AP_PASSWORD_DEFAULT[0] != '\0' ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
}

static bool provisioning_pending(void) {
    return s_provision_result.pending;
}

static void set_provisioning_result(
    bool pending,
    bool success,
    bool restart_required,
    app_wifi_provision_status_t status,
    const char *code,
    const char *message
) {
    s_provision_result.pending = pending;
    s_provision_result.success = success;
    s_provision_result.restart_required = restart_required;
    s_provision_result.status = status;
    copy_string(s_provision_result.code, sizeof(s_provision_result.code), code);
    copy_string(s_provision_result.message, sizeof(s_provision_result.message), message);
}

static void init_provisioning_result(void) {
    memset(&s_provision_result, 0, sizeof(s_provision_result));
    set_provisioning_result(
        false,
        false,
        false,
        APP_WIFI_PROVISION_STATUS_IDLE,
        "idle",
        "Nenhuma validacao de Wi-Fi em andamento."
    );
}

static bool is_auth_failure_reason(wifi_err_reason_t reason) {
    return reason == WIFI_REASON_AUTH_FAIL ||
        reason == WIFI_REASON_AUTH_EXPIRE ||
        reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static bool is_ssid_not_found_reason(wifi_err_reason_t reason) {
    return reason == WIFI_REASON_NO_AP_FOUND ||
        reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD ||
        reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
        reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY;
}

static void provisioning_timeout_task(void *arg) {
    uint32_t generation = (uint32_t)(uintptr_t)arg;

    vTaskDelay(pdMS_TO_TICKS(APP_WIFI_PROVISION_TIMEOUT_MS));

    if (generation == s_provision_generation && provisioning_pending()) {
        ESP_LOGW(LOG_TAG_WIFI_MANAGER, "Timeout validando o Wi-Fi informado");
        set_provisioning_result(
            false,
            false,
            false,
            APP_WIFI_PROVISION_STATUS_TIMEOUT,
            "wifi_timeout",
            "Nao foi possivel conectar a rede Wi-Fi informada. Verifique o nome da rede e a senha e tente novamente."
        );
    }

    vTaskDelete(NULL);
}

static void start_provisioning_timeout_watchdog(void) {
    uint32_t generation = s_provision_generation;

    if (xTaskCreate(
            provisioning_timeout_task,
            "wifi_prov_timeout",
            2048,
            (void *)(uintptr_t)generation,
            4,
            NULL
        ) != pdPASS) {
        ESP_LOGW(LOG_TAG_WIFI_MANAGER, "Nao foi possivel iniciar o watchdog de provisionamento");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    const wifi_event_sta_disconnected_t *disconnected = event_data;
    wifi_err_reason_t reason = disconnected != NULL ? disconnected->reason : WIFI_REASON_UNSPECIFIED;
    (void)arg;
    (void)base;

    if (event_id == WIFI_EVENT_STA_START) {
        if (s_config.sta_enabled && s_config.sta_ssid[0] != '\0') {
            ESP_LOGI(LOG_TAG_WIFI_MANAGER, "Wi-Fi STA iniciado");
            esp_wifi_connect();
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;

        if (provisioning_pending()) {
            if (is_auth_failure_reason(reason)) {
                ESP_LOGW(LOG_TAG_WIFI_MANAGER, "Falha de autenticacao no Wi-Fi informado");
                set_provisioning_result(
                    false,
                    false,
                    false,
                    APP_WIFI_PROVISION_STATUS_AUTH_FAILED,
                    "wifi_auth_failed",
                    "Nao foi possivel conectar a rede Wi-Fi informada. Verifique o nome da rede e a senha e tente novamente."
                );
                return;
            }

            if (is_ssid_not_found_reason(reason)) {
                ESP_LOGW(LOG_TAG_WIFI_MANAGER, "SSID informado nao foi encontrado");
                set_provisioning_result(
                    false,
                    false,
                    false,
                    APP_WIFI_PROVISION_STATUS_SSID_NOT_FOUND,
                    "wifi_ssid_not_found",
                    "A rede Wi-Fi informada nao foi encontrada. Verifique o nome da rede e tente novamente."
                );
                return;
            }

            if (s_retry_count < s_config.max_retry) {
                s_retry_count++;
                ESP_LOGW(
                    LOG_TAG_WIFI_MANAGER,
                    "Validando Wi-Fi informado, nova tentativa (%d/%d)",
                    s_retry_count,
                    s_config.max_retry
                );
                esp_wifi_connect();
                return;
            }

            ESP_LOGW(LOG_TAG_WIFI_MANAGER, "Nao foi possivel validar o Wi-Fi informado");
            set_provisioning_result(
                false,
                false,
                false,
                APP_WIFI_PROVISION_STATUS_ERROR,
                "wifi_connection_failed",
                "Nao foi possivel conectar a rede Wi-Fi informada. Verifique o nome da rede e a senha e tente novamente."
            );
            return;
        }

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

        if (provisioning_pending()) {
            esp_err_t save_err = comm_storage_save_provisioning(
                s_pending_device_id,
                s_pending_wifi_ssid,
                s_pending_wifi_password
            );

            if (save_err != ESP_OK) {
                ESP_LOGE(LOG_TAG_WIFI_MANAGER, "Falha ao salvar o Wi-Fi validado: %s", esp_err_to_name(save_err));
                set_provisioning_result(
                    false,
                    false,
                    false,
                    APP_WIFI_PROVISION_STATUS_ERROR,
                    "wifi_save_failed",
                    "A IHM conseguiu entrar na rede, mas nao conseguiu salvar a configuracao. Tente novamente."
                );
                return;
            }

            ESP_LOGI(LOG_TAG_WIFI_MANAGER, "Wi-Fi validado com sucesso; aguardando reinicio controlado");
            set_provisioning_result(
                false,
                true,
                true,
                APP_WIFI_PROVISION_STATUS_SUCCESS,
                "wifi_connected",
                "Conexao Wi-Fi confirmada. A IHM vai reiniciar para concluir a configuracao."
            );
        }

        ESP_LOGI(LOG_TAG_WIFI_MANAGER, "Wi-Fi conectado, iniciando SNTP e MQTT");
        time_sync_notify_network_ready();
        ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_start());
    }
}

esp_err_t wifi_manager_start(const app_wifi_config_t *config) {
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t sta_config = {0};
    wifi_config_t ap_config = {0};
    wifi_mode_t mode = WIFI_MODE_AP;
    esp_err_t loop_err = ESP_OK;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(s_config));
    s_retry_count = 0;
    s_sta_connected = false;
    s_ap_active = false;
    init_provisioning_result();

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
        fill_sta_config(&sta_config, &s_config);
    }

    fill_ap_config(&ap_config);

    ESP_LOGI(
        LOG_TAG_WIFI_MANAGER,
        "Inicializando Wi-Fi: mode=%s STA=%s",
        mode == WIFI_MODE_APSTA ? "APSTA" : "AP",
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

esp_err_t wifi_manager_begin_provisioning_attempt(
    const char *device_id,
    const char *wifi_ssid,
    const char *wifi_password
) {
    wifi_config_t sta_config = {0};

    if (
        !s_initialized ||
        device_id == NULL || device_id[0] == '\0' ||
        wifi_ssid == NULL || wifi_ssid[0] == '\0'
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    copy_string(s_pending_device_id, sizeof(s_pending_device_id), device_id);
    copy_string(s_pending_wifi_ssid, sizeof(s_pending_wifi_ssid), wifi_ssid);
    copy_string(s_pending_wifi_password, sizeof(s_pending_wifi_password), wifi_password);

    copy_string(s_provision_result.device_id, sizeof(s_provision_result.device_id), device_id);
    copy_string(s_provision_result.wifi_ssid, sizeof(s_provision_result.wifi_ssid), wifi_ssid);

    s_config.sta_enabled = true;
    s_config.max_retry = APP_WIFI_MAXIMUM_RETRY;
    copy_string(s_config.sta_ssid, sizeof(s_config.sta_ssid), wifi_ssid);
    copy_string(s_config.sta_password, sizeof(s_config.sta_password), wifi_password);
    fill_sta_config(&sta_config, &s_config);

    s_provision_generation++;
    s_retry_count = 0;
    s_sta_connected = false;

    set_provisioning_result(
        true,
        false,
        false,
        APP_WIFI_PROVISION_STATUS_CONNECTING,
        "wifi_connecting",
        "Validando a rede Wi-Fi informada. Aguarde a confirmacao da IHM."
    );

    ESP_LOGI(LOG_TAG_WIFI_MANAGER, "Iniciando validacao do Wi-Fi informado: ssid=%s", wifi_ssid);

    start_provisioning_timeout_watchdog();
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());

    return ESP_OK;
}

void wifi_manager_get_provisioning_result(app_wifi_provision_result_t *result) {
    if (result == NULL) {
        return;
    }

    memcpy(result, &s_provision_result, sizeof(*result));
}

bool wifi_manager_is_connected(void) {
    return s_sta_connected;
}

bool wifi_manager_is_ap_active(void) {
    return s_ap_active;
}
