#include "local_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"

#include "cJSON.h"

#include "app_config.h"
#include "comm_storage.h"
#include "ihm_mqtt_adapter.h"
#include "log_tags.h"
#include "mqtt_manager.h"
#include "protocol_json.h"
#include "schedule_manager.h"
#include "wifi_manager.h"

static httpd_handle_t s_http_server;
static bool s_restart_scheduled;

static void set_json_headers(httpd_req_t *request) {
    httpd_resp_set_type(request, "application/json");

    if (APP_LOCAL_SERVER_CORS_ENABLED) {
        httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(request, "Access-Control-Allow-Headers", "Content-Type, Authorization");
        httpd_resp_set_hdr(request, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    }
}

static esp_err_t send_json_string(httpd_req_t *request, const char *status, const char *json) {
    set_json_headers(request);
    httpd_resp_set_status(request, status);
    return httpd_resp_sendstr(request, json != NULL ? json : "{}");
}

static esp_err_t send_owned_json(httpd_req_t *request, const char *status, char *json) {
    esp_err_t err = send_json_string(request, status, json);
    free(json);
    return err;
}

static esp_err_t options_handler(httpd_req_t *request) {
    set_json_headers(request);
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t read_request_body(httpd_req_t *request, char **out_body) {
    int total_len = request->content_len;
    int received = 0;
    char *body = NULL;

    if (out_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (total_len <= 0 || total_len > APP_HTTP_BODY_BUFFER_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    body = calloc((size_t)total_len + 1, sizeof(char));
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    while (received < total_len) {
        int chunk = httpd_req_recv(request, body + received, total_len - received);
        if (chunk <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += chunk;
    }

    *out_body = body;
    return ESP_OK;
}

static bool contains_non_space(const char *text) {
    size_t index = 0;

    if (text == NULL) {
        return false;
    }

    for (index = 0; text[index] != '\0'; ++index) {
        if (!isspace((unsigned char)text[index])) {
            return true;
        }
    }

    return false;
}

static bool device_id_is_valid(const char *device_id) {
    size_t index = 0;
    size_t len = 0;

    if (device_id == NULL) {
        return false;
    }

    len = strlen(device_id);
    if (len == 0 || len > APP_DEVICE_ID_MAX_LEN) {
        return false;
    }

    for (index = 0; index < len; ++index) {
        if (isspace((unsigned char)device_id[index])) {
            return false;
        }
    }

    return true;
}

static app_connection_mode_t local_connection_mode(void) {
    return wifi_manager_is_ap_active()
        ? APP_CONNECTION_MODE_LOCAL_AP
        : APP_CONNECTION_MODE_LOCAL_LAN;
}

static esp_err_t load_current_device_id(char *device_id, size_t device_id_len) {
    app_mqtt_config_t mqtt_config = {0};
    esp_err_t err = comm_storage_load_mqtt_config(&mqtt_config);

    if (err != ESP_OK) {
        return err;
    }

    strncpy(device_id, mqtt_config.device_id, device_id_len - 1);
    return ESP_OK;
}

static esp_err_t build_local_state(device_state_t *state) {
    esp_err_t err = ihm_mqtt_adapter_get_state(state);

    if (err != ESP_OK) {
        return err;
    }

    state->device_online = true;
    state->connection_mode = local_connection_mode();
    return ESP_OK;
}

static const char *diagnostics_transport_status(void) {
    if (mqtt_manager_is_connected()) {
        return "connected";
    }

    if (wifi_manager_is_connected() || wifi_manager_is_ap_active()) {
        return "degraded";
    }

    return "error";
}

static void build_connection_summary(char *summary, size_t summary_len) {
    if (mqtt_manager_is_connected()) {
        snprintf(
            summary,
            summary_len,
            "Cloud MQTT conectado. Fallback local segue disponivel pelo AP da IHM."
        );
    } else if (wifi_manager_is_connected()) {
        snprintf(
            summary,
            summary_len,
            "Wi-Fi do estabelecimento conectado, mas a nuvem nao esta acessivel. Use o fallback local."
        );
    } else if (wifi_manager_is_ap_active()) {
        snprintf(
            summary,
            summary_len,
            "Wi-Fi de provisionamento/fallback ativo. A IHM aguarda provisionamento ou acesso local."
        );
    } else {
        snprintf(summary, summary_len, "Conectividade indisponivel no momento.");
    }
}

static void restart_after_provisioning(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(APP_HTTP_RESTART_DELAY_MS));
    ESP_LOGW(LOG_TAG_LOCAL_SERVER, "Reiniciando IHM para aplicar provisionamento");
    esp_restart();
}

static void schedule_restart_once(void) {
    if (s_restart_scheduled) {
        return;
    }

    s_restart_scheduled = true;
    xTaskCreate(restart_after_provisioning, "prov_restart", 2048, NULL, 4, NULL);
}

static esp_err_t ping_handler(httpd_req_t *request) {
    char device_id[APP_DEVICE_ID_MAX_LEN + 1] = {0};
    char response[256] = {0};

    ESP_ERROR_CHECK_WITHOUT_ABORT(load_current_device_id(device_id, sizeof(device_id)));
    snprintf(
        response,
        sizeof(response),
        "{\"ok\":true,\"message\":\"Servidor local ativo\",\"timestamp\":\"%ld\",\"deviceId\":\"%s\"}",
        (long)time(NULL),
        device_id
    );

    return send_json_string(request, "200 OK", response);
}

static esp_err_t status_handler(httpd_req_t *request) {
    char device_id[APP_DEVICE_ID_MAX_LEN + 1] = {0};
    device_state_t state = {0};
    char *json = NULL;

    if (load_current_device_id(device_id, sizeof(device_id)) != ESP_OK || build_local_state(&state) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"status_unavailable\"}");
    }

    if (protocol_json_build_status(device_id, &state, &json) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"status_unavailable\"}");
    }

    return send_owned_json(request, "200 OK", json);
}

static esp_err_t state_handler(httpd_req_t *request) {
    char device_id[APP_DEVICE_ID_MAX_LEN + 1] = {0};
    device_state_t state = {0};
    char *json = NULL;

    if (load_current_device_id(device_id, sizeof(device_id)) != ESP_OK || build_local_state(&state) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"state_unavailable\"}");
    }

    if (protocol_json_build_state(device_id, &state, &json) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"state_unavailable\"}");
    }

    return send_owned_json(request, "200 OK", json);
}

static esp_err_t capabilities_handler(httpd_req_t *request) {
    char device_id[APP_DEVICE_ID_MAX_LEN + 1] = {0};
    device_capabilities_t capabilities = {0};
    char *json = NULL;

    if (
        load_current_device_id(device_id, sizeof(device_id)) != ESP_OK ||
        ihm_mqtt_adapter_get_capabilities(&capabilities) != ESP_OK
    ) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"capabilities_unavailable\"}");
    }

    if (protocol_json_build_capabilities(device_id, &capabilities, &json) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"capabilities_unavailable\"}");
    }

    return send_owned_json(request, "200 OK", json);
}

static esp_err_t diagnostics_handler(httpd_req_t *request) {
    char device_id[APP_DEVICE_ID_MAX_LEN + 1] = {0};
    char summary[APP_MESSAGE_MAX_LEN + 1] = {0};
    char *json = NULL;

    if (load_current_device_id(device_id, sizeof(device_id)) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"diagnostics_unavailable\"}");
    }

    build_connection_summary(summary, sizeof(summary));

    if (
        protocol_json_build_diagnostics(
            device_id,
            summary,
            diagnostics_transport_status(),
            NULL,
            &json
        ) != ESP_OK
    ) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"diagnostics_unavailable\"}");
    }

    return send_owned_json(request, "200 OK", json);
}

static esp_err_t commands_post_handler(httpd_req_t *request) {
    char device_id[APP_DEVICE_ID_MAX_LEN + 1] = {0};
    char *body = NULL;
    char *json = NULL;
    app_command_t command = {0};
    command_result_t result = {0};
    device_state_t state = {0};
    esp_err_t err = ESP_OK;

    err = read_request_body(request, &body);
    if (err != ESP_OK) {
        return send_json_string(request, "400 Bad Request", "{\"error\":\"invalid_request_body\"}");
    }

    if (protocol_json_parse_command(body, &command) != ESP_OK) {
        free(body);
        return send_json_string(request, "400 Bad Request", "{\"error\":\"invalid_command\"}");
    }

    free(body);
    if (load_current_device_id(device_id, sizeof(device_id)) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"device_id_unavailable\"}");
    }

    if (command.device_id[0] != '\0' && strcmp(command.device_id, device_id) != 0) {
        return send_json_string(request, "400 Bad Request", "{\"error\":\"device_id_mismatch\"}");
    }

    if (command.device_id[0] == '\0') {
        strncpy(command.device_id, device_id, sizeof(command.device_id) - 1);
    }
    command.source = APP_COMMAND_SOURCE_INTERNAL;

    err = ihm_mqtt_adapter_execute_command(&command, &result);
    ESP_ERROR_CHECK_WITHOUT_ABORT(build_local_state(&state));

    if (protocol_json_build_command_ack(device_id, &command, &result, &state, &json) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"ack_unavailable\"}");
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_state());
    if (command.type == APP_COMMAND_REQUEST_CAPABILITIES) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_capabilities());
    }

    return send_owned_json(request, "200 OK", json);
}

static esp_err_t schedules_get_handler(httpd_req_t *request) {
    char device_id[APP_DEVICE_ID_MAX_LEN + 1] = {0};
    char *json = NULL;

    if (load_current_device_id(device_id, sizeof(device_id)) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"device_id_unavailable\"}");
    }
    if (schedule_manager_build_payload(device_id, &json) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"schedules_unavailable\"}");
    }

    return send_owned_json(request, "200 OK", json);
}

static esp_err_t schedules_post_handler(httpd_req_t *request) {
    char error_code[APP_CODE_MAX_LEN + 1] = {0};
    char error_message[APP_MESSAGE_MAX_LEN + 1] = {0};
    char device_id[APP_DEVICE_ID_MAX_LEN + 1] = {0};
    char *body = NULL;
    char *json = NULL;
    esp_err_t err = read_request_body(request, &body);

    if (err != ESP_OK) {
        return send_json_string(request, "400 Bad Request", "{\"error\":\"invalid_request_body\"}");
    }

    err = schedule_manager_handle_mqtt_payload(
        body,
        error_code,
        sizeof(error_code),
        error_message,
        sizeof(error_message)
    );
    free(body);

    if (err != ESP_OK) {
        return send_json_string(request, "400 Bad Request", "{\"error\":\"invalid_schedules\"}");
    }

    if (load_current_device_id(device_id, sizeof(device_id)) != ESP_OK) {
        return send_json_string(request, "500 Internal Server Error", "{\"error\":\"device_id_unavailable\"}");
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(schedule_manager_build_payload(device_id, &json));
    ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_schedules());

    return send_owned_json(request, "200 OK", json);
}

static esp_err_t provisioning_post_handler(httpd_req_t *request) {
    cJSON *root = NULL;
    cJSON *device_id_item = NULL;
    cJSON *wifi_ssid_item = NULL;
    cJSON *wifi_password_item = NULL;
    char *body = NULL;
    char response[512] = {0};
    const char *device_id = NULL;
    const char *wifi_ssid = NULL;
    const char *wifi_password = "";
    esp_err_t err = read_request_body(request, &body);

    if (err != ESP_OK) {
        return send_json_string(
            request,
            "400 Bad Request",
            "{\"ok\":false,\"accepted\":false,\"message\":\"Corpo de provisionamento invalido.\"}"
        );
    }

    root = cJSON_Parse(body);
    free(body);

    if (root == NULL) {
        return send_json_string(
            request,
            "400 Bad Request",
            "{\"ok\":false,\"accepted\":false,\"message\":\"JSON de provisionamento invalido.\"}"
        );
    }

    device_id_item = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
    wifi_ssid_item = cJSON_GetObjectItemCaseSensitive(root, "wifiSsid");
    wifi_password_item = cJSON_GetObjectItemCaseSensitive(root, "wifiPassword");

    device_id = cJSON_IsString(device_id_item) ? device_id_item->valuestring : NULL;
    wifi_ssid = cJSON_IsString(wifi_ssid_item) ? wifi_ssid_item->valuestring : NULL;
    if (cJSON_IsString(wifi_password_item)) {
        wifi_password = wifi_password_item->valuestring;
    }

    if (!device_id_is_valid(device_id) || !contains_non_space(wifi_ssid)) {
        cJSON_Delete(root);
        return send_json_string(
            request,
            "400 Bad Request",
            "{\"ok\":false,\"accepted\":false,\"message\":\"Informe um ID valido e o nome da rede Wi-Fi.\"}"
        );
    }

    err = comm_storage_save_provisioning(device_id, wifi_ssid, wifi_password);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGE(LOG_TAG_LOCAL_SERVER, "Falha ao salvar provisionamento: %s", esp_err_to_name(err));
        return send_json_string(
            request,
            "500 Internal Server Error",
            "{\"ok\":false,\"accepted\":false,\"message\":\"Nao foi possivel salvar o provisionamento na IHM.\"}"
        );
    }

    snprintf(
        response,
        sizeof(response),
        "{\"ok\":true,\"accepted\":true,\"message\":\"Provisionamento aceito. A IHM vai reiniciar e tentar entrar no Wi-Fi informado.\",\"deviceId\":\"%s\",\"firmwareVersion\":\"%s\"}",
        device_id,
        APP_FIRMWARE_VERSION
    );

    schedule_restart_once();
    return send_json_string(request, "200 OK", response);
}

static esp_err_t register_endpoint(const httpd_uri_t *route) {
    return httpd_register_uri_handler(s_http_server, route);
}

esp_err_t local_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    size_t index = 0;
    httpd_uri_t routes[] = {
        {.uri = "/api/v1/ping", .method = HTTP_GET, .handler = ping_handler, .user_ctx = NULL},
        {.uri = "/api/v1/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL},
        {.uri = "/api/v1/state", .method = HTTP_GET, .handler = state_handler, .user_ctx = NULL},
        {.uri = "/api/v1/capabilities", .method = HTTP_GET, .handler = capabilities_handler, .user_ctx = NULL},
        {.uri = "/api/v1/diagnostics", .method = HTTP_GET, .handler = diagnostics_handler, .user_ctx = NULL},
        {.uri = "/api/v1/schedules", .method = HTTP_GET, .handler = schedules_get_handler, .user_ctx = NULL},
        {.uri = "/api/v1/commands", .method = HTTP_POST, .handler = commands_post_handler, .user_ctx = NULL},
        {.uri = "/api/v1/schedules", .method = HTTP_POST, .handler = schedules_post_handler, .user_ctx = NULL},
        {.uri = "/api/v1/provisioning", .method = HTTP_POST, .handler = provisioning_post_handler, .user_ctx = NULL},
        {.uri = "/api/v1/commands", .method = HTTP_OPTIONS, .handler = options_handler, .user_ctx = NULL},
        {.uri = "/api/v1/schedules", .method = HTTP_OPTIONS, .handler = options_handler, .user_ctx = NULL},
        {.uri = "/api/v1/provisioning", .method = HTTP_OPTIONS, .handler = options_handler, .user_ctx = NULL},
        {.uri = "/api/v1/ping", .method = HTTP_OPTIONS, .handler = options_handler, .user_ctx = NULL},
        {.uri = "/api/v1/status", .method = HTTP_OPTIONS, .handler = options_handler, .user_ctx = NULL},
        {.uri = "/api/v1/state", .method = HTTP_OPTIONS, .handler = options_handler, .user_ctx = NULL},
        {.uri = "/api/v1/capabilities", .method = HTTP_OPTIONS, .handler = options_handler, .user_ctx = NULL},
        {.uri = "/api/v1/diagnostics", .method = HTTP_OPTIONS, .handler = options_handler, .user_ctx = NULL},
    };

    if (s_http_server != NULL) {
        return ESP_OK;
    }

    config.server_port = APP_LOCAL_SERVER_PORT_DEFAULT;
    config.max_uri_handlers = 24;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), LOG_TAG_LOCAL_SERVER, "httpd start");

    for (index = 0; index < sizeof(routes) / sizeof(routes[0]); ++index) {
        ESP_RETURN_ON_ERROR(register_endpoint(&routes[index]), LOG_TAG_LOCAL_SERVER, "register endpoint");
    }

    ESP_LOGI(
        LOG_TAG_LOCAL_SERVER,
        "Servidor local iniciado na porta %d para provisionamento e fallback",
        APP_LOCAL_SERVER_PORT_DEFAULT
    );

    return ESP_OK;
}
