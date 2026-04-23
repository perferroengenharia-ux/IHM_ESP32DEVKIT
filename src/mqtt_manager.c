#include "mqtt_manager.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "certs.h"
#include "ihm_mqtt_adapter.h"
#include "log_tags.h"
#include "protocol_json.h"
#include "protocol_topics.h"
#include "schedule_manager.h"

static app_mqtt_config_t s_config;
static protocol_topic_bundle_t s_topics;
static esp_mqtt_client_handle_t s_client;
static bool s_started;
static bool s_connected;

static esp_err_t publish_json(const char *topic, char *json, int qos, bool retain) {
    int message_id = 0;

    if (topic == NULL || json == NULL) {
        free(json);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_connected || s_client == NULL) {
        free(json);
        return ESP_ERR_INVALID_STATE;
    }

    message_id = esp_mqtt_client_publish(s_client, topic, json, 0, qos, retain);
    free(json);

    return message_id >= 0 ? ESP_OK : ESP_FAIL;
}

static void log_command_result(const app_command_t *command, const command_result_t *result, esp_err_t err) {
    ESP_LOGI(
        LOG_TAG_MQTT_MANAGER,
        "Comando processado: id=%s type=%s accepted=%d applied=%d status=%s code=%s err=%s msg=%s",
        command != NULL ? command->id : "",
        command != NULL ? protocol_json_command_type_to_string(command->type) : "",
        result != NULL ? result->accepted : 0,
        result != NULL ? result->applied : 0,
        result != NULL ? (result->status == APP_LAST_COMMAND_APPLIED ? "applied" : (result->status == APP_LAST_COMMAND_FAILED ? "failed" : "sending")) : "unknown",
        result != NULL && result->code[0] != '\0' ? result->code : "none",
        esp_err_to_name(err),
        result != NULL && result->message[0] != '\0' ? result->message : "sem mensagem"
    );
}

esp_err_t mqtt_manager_publish_state(void) {
    device_state_t state = {0};
    char *status_json = NULL;
    char *state_json = NULL;

    ESP_ERROR_CHECK_WITHOUT_ABORT(ihm_mqtt_adapter_get_state(&state));
    state.connection_mode = s_connected ? APP_CONNECTION_MODE_CLOUD : APP_CONNECTION_MODE_LOCAL_LAN;
    ESP_ERROR_CHECK_WITHOUT_ABORT(protocol_json_build_status(s_config.device_id, &state, &status_json));
    ESP_ERROR_CHECK_WITHOUT_ABORT(protocol_json_build_state(s_config.device_id, &state, &state_json));
    ESP_ERROR_CHECK_WITHOUT_ABORT(publish_json(s_topics.status, status_json, 1, false));
    ESP_ERROR_CHECK_WITHOUT_ABORT(publish_json(s_topics.state, state_json, 1, false));

    return ESP_OK;
}

esp_err_t mqtt_manager_publish_capabilities(void) {
    device_capabilities_t capabilities = {0};
    char *capabilities_json = NULL;

    ESP_ERROR_CHECK_WITHOUT_ABORT(ihm_mqtt_adapter_get_capabilities(&capabilities));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        protocol_json_build_capabilities(s_config.device_id, &capabilities, &capabilities_json)
    );

    return publish_json(s_topics.capabilities, capabilities_json, 1, true);
}

esp_err_t mqtt_manager_publish_schedules(void) {
    char *schedules_json = NULL;
    esp_err_t err = schedule_manager_build_payload(s_config.device_id, &schedules_json);

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(LOG_TAG_MQTT_MANAGER, "Publicando snapshot de schedules em %s", s_topics.schedules);
    return publish_json(s_topics.schedules, schedules_json, 1, true);
}

esp_err_t mqtt_manager_publish_full_snapshot(void) {
    ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_state());
    ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_capabilities());
    ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_schedules());
    return ESP_OK;
}

static esp_err_t publish_ack(const app_command_t *command, const command_result_t *result) {
    device_state_t state = {0};
    char *json = NULL;
    const char *topic = result->status == APP_LAST_COMMAND_FAILED ? s_topics.errors : s_topics.events;

    ESP_ERROR_CHECK_WITHOUT_ABORT(ihm_mqtt_adapter_get_state(&state));
    state.connection_mode = APP_CONNECTION_MODE_CLOUD;
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        protocol_json_build_command_ack(s_config.device_id, command, result, &state, &json)
    );

    ESP_LOGI(
        LOG_TAG_MQTT_MANAGER,
        "Publicando ack de comando em %s para id=%s type=%s",
        topic,
        command->id,
        protocol_json_command_type_to_string(command->type)
    );

    return publish_json(topic, json, 1, false);
}

static void publish_structured_error(const char *code, const char *message) {
    char *json = NULL;

    if (protocol_json_build_error(s_config.device_id, code, message, &json) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(publish_json(s_topics.errors, json, 1, false));
    }
}

static void handle_command_payload(const char *topic, const char *payload) {
    app_command_t command = {0};
    command_result_t result = {0};
    esp_err_t command_err = ESP_OK;

    ESP_LOGI(LOG_TAG_MQTT_MANAGER, "Payload recebido em %s: %s", topic, payload);

    if (protocol_json_parse_command(payload, &command) != ESP_OK) {
        ESP_LOGW(LOG_TAG_MQTT_MANAGER, "Falha no parse do comando MQTT");
        publish_structured_error("invalid_command", "Payload de comando invalido");
        return;
    }

    ESP_LOGI(
        LOG_TAG_MQTT_MANAGER,
        "Comando reconhecido: id=%s type=%s deviceId=%s",
        command.id,
        protocol_json_command_type_to_string(command.type),
        command.device_id
    );

    if (command.device_id[0] != '\0' && strcmp(command.device_id, s_config.device_id) != 0) {
        ESP_LOGW(
            LOG_TAG_MQTT_MANAGER,
            "Comando ignorado por deviceId divergente: recebido=%s esperado=%s",
            command.device_id,
            s_config.device_id
        );
        return;
    }

    if (command.device_id[0] == '\0') {
        strncpy(command.device_id, s_config.device_id, sizeof(command.device_id) - 1);
    }

    command_err = ihm_mqtt_adapter_execute_command(&command, &result);
    log_command_result(&command, &result, command_err);
    ESP_ERROR_CHECK_WITHOUT_ABORT(publish_ack(&command, &result));
    ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_state());

    if (command.type == APP_COMMAND_REQUEST_CAPABILITIES) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_capabilities());
    }
}

static void handle_schedules_payload(const char *topic, const char *payload) {
    char error_code[APP_CODE_MAX_LEN + 1] = {0};
    char error_message[APP_MESSAGE_MAX_LEN + 1] = {0};
    esp_err_t err = ESP_OK;

    ESP_LOGI(LOG_TAG_MQTT_MANAGER, "Payload recebido em %s: %s", topic, payload);

    if (protocol_json_source_equals(payload, "ihm")) {
        ESP_LOGI(
            LOG_TAG_MQTT_MANAGER,
            "Snapshot de schedules publicado pela propria IHM ignorado para evitar loop MQTT"
        );
        return;
    }

    err = schedule_manager_handle_mqtt_payload(
        payload,
        error_code,
        sizeof(error_code),
        error_message,
        sizeof(error_message)
    );

    if (err != ESP_OK) {
        ESP_LOGW(
            LOG_TAG_MQTT_MANAGER,
            "Falha ao salvar schedules: code=%s err=%s msg=%s",
            error_code[0] != '\0' ? error_code : "schedule_error",
            esp_err_to_name(err),
            error_message[0] != '\0' ? error_message : "erro nao especificado"
        );
        publish_structured_error(
            error_code[0] != '\0' ? error_code : "schedule_error",
            error_message[0] != '\0' ? error_message : "Falha ao salvar schedules"
        );
        return;
    }

    ESP_LOGI(LOG_TAG_MQTT_MANAGER, "Schedules recebidos e persistidos com sucesso");
    ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_schedules());
}

static void mqtt_event_handler(void *handler, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    (void)handler;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            int command_sub_id = 0;
            int schedule_sub_id = 0;

            s_connected = true;
            ESP_LOGI(LOG_TAG_MQTT_MANAGER, "MQTT conectado");
            command_sub_id = esp_mqtt_client_subscribe(s_client, s_topics.commands, 1);
            schedule_sub_id = esp_mqtt_client_subscribe(s_client, s_topics.schedules, 1);
            ESP_LOGI(
                LOG_TAG_MQTT_MANAGER,
                "Subscribe realizado: commands=%s (msg_id=%d) schedules=%s (msg_id=%d)",
                s_topics.commands,
                command_sub_id,
                s_topics.schedules,
                schedule_sub_id
            );
            ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_full_snapshot());
            break;
        }

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(LOG_TAG_MQTT_MANAGER, "Subscribe confirmado pelo broker: msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(LOG_TAG_MQTT_MANAGER, "MQTT desconectado");
            break;

        case MQTT_EVENT_DATA: {
            char *topic = calloc((size_t)event->topic_len + 1, 1);
            char *payload = calloc((size_t)event->data_len + 1, 1);

            if (topic == NULL || payload == NULL) {
                free(topic);
                free(payload);
                break;
            }

            memcpy(topic, event->topic, (size_t)event->topic_len);
            memcpy(payload, event->data, (size_t)event->data_len);

            if (strcmp(topic, s_topics.commands) == 0) {
                handle_command_payload(topic, payload);
            } else if (strcmp(topic, s_topics.schedules) == 0) {
                handle_schedules_payload(topic, payload);
            } else {
                ESP_LOGW(LOG_TAG_MQTT_MANAGER, "Topico MQTT recebido sem handler: %s", topic);
            }

            free(topic);
            free(payload);
            break;
        }

        case MQTT_EVENT_ERROR:
            s_connected = false;
            ESP_LOGE(LOG_TAG_MQTT_MANAGER, "Erro MQTT");
            break;

        default:
            break;
    }
}

esp_err_t mqtt_manager_configure(const app_mqtt_config_t *config) {
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_config, config, sizeof(s_config));
    return protocol_topics_build(&s_config, &s_topics);
}

esp_err_t mqtt_manager_start(void) {
    esp_mqtt_client_config_t cfg = {0};
    bool tls_required = false;

    if (s_started) {
        return ESP_OK;
    }

    if (!s_config.enabled || s_config.broker_uri[0] == '\0') {
        ESP_LOGW(LOG_TAG_MQTT_MANAGER, "MQTT desabilitado ou sem broker configurado");
        return ESP_OK;
    }

    tls_required = s_config.use_tls ||
        s_config.port == 8883 ||
        strncmp(s_config.broker_uri, "mqtts://", 8) == 0 ||
        strncmp(s_config.broker_uri, "wss://", 6) == 0;

    cfg.broker.address.uri = s_config.broker_uri;
    cfg.broker.address.port = s_config.port;
    cfg.credentials.username = s_config.username[0] != '\0' ? s_config.username : NULL;
    cfg.credentials.authentication.password = s_config.password[0] != '\0' ? s_config.password : NULL;
    cfg.session.keepalive = s_config.keepalive_sec;

    if (tls_required) {
        cfg.broker.verification.certificate = APP_ROOT_CA_PEM;
    }

    ESP_LOGI(
        LOG_TAG_MQTT_MANAGER,
        "Inicializando MQTT: broker=%s port=%u tls=%d commands=%s schedules=%s",
        s_config.broker_uri,
        s_config.port,
        tls_required,
        s_topics.commands,
        s_topics.schedules
    );

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
    s_started = true;

    return ESP_OK;
}

bool mqtt_manager_is_connected(void) {
    return s_connected;
}
