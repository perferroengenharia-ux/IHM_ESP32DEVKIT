#include "schedule_manager.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

#include "ihm_mqtt_adapter.h"
#include "log_tags.h"
#include "mqtt_manager.h"
#include "protocol_json.h"
#include "time_sync.h"

static app_schedule_store_t s_store;
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static bool s_waiting_for_time_logged;

static esp_err_t save_store_locked(void) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NVS_NAMESPACE_SCHEDULES, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, APP_NVS_KEY_SCHEDULE_STORE, &s_store, sizeof(s_store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t load_store_locked(void) {
    nvs_handle_t handle = 0;
    size_t required = sizeof(s_store);
    esp_err_t err = nvs_open(APP_NVS_NAMESPACE_SCHEDULES, NVS_READONLY, &handle);

    memset(&s_store, 0, sizeof(s_store));
    s_store.timezone_offset_minutes = APP_SCHEDULE_TZ_OFFSET_MIN_DEFAULT;

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(handle, APP_NVS_KEY_SCHEDULE_STORE, &s_store, &required);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_store, 0, sizeof(s_store));
        s_store.timezone_offset_minutes = APP_SCHEDULE_TZ_OFFSET_MIN_DEFAULT;
        return ESP_OK;
    }

    if (err != ESP_OK) {
        return err;
    }

    if (required != sizeof(s_store)) {
        memset(&s_store, 0, sizeof(s_store));
        s_store.timezone_offset_minutes = APP_SCHEDULE_TZ_OFFSET_MIN_DEFAULT;
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_store.timezone_offset_minutes < -720 || s_store.timezone_offset_minutes > 840) {
        s_store.timezone_offset_minutes = APP_SCHEDULE_TZ_OFFSET_MIN_DEFAULT;
    }

    if (s_store.count > APP_SCHEDULE_MAX_COUNT) {
        s_store.count = APP_SCHEDULE_MAX_COUNT;
    }

    return ESP_OK;
}

static uint64_t build_minute_key(const struct tm *local_tm) {
    return ((uint64_t)(local_tm->tm_year + 1900) * 100000000ULL) +
        ((uint64_t)(local_tm->tm_mon + 1) * 1000000ULL) +
        ((uint64_t)local_tm->tm_mday * 10000ULL) +
        ((uint64_t)local_tm->tm_hour * 100ULL) +
        (uint64_t)local_tm->tm_min;
}

static void format_utc_timestamp(time_t value, char *buffer, size_t len) {
    struct tm tm_value = {0};
    time_t safe_value = value > 0 ? value : time(NULL);

    gmtime_r(&safe_value, &tm_value);
    strftime(buffer, len, "%Y-%m-%dT%H:%M:%SZ", &tm_value);
}

static bool parse_one_shot_date(const char *text, int *year, int *month, int *day) {
    if (text == NULL || year == NULL || month == NULL || day == NULL) {
        return false;
    }

    if (sscanf(text, "%4d-%2d-%2d", year, month, day) != 3) {
        return false;
    }

    return *month >= 1 && *month <= 12 && *day >= 1 && *day <= 31;
}

static bool schedule_matches_now(const app_schedule_t *schedule, const struct tm *local_tm) {
    int year = 0;
    int month = 0;
    int day = 0;

    if (!schedule->enabled) {
        return false;
    }

    if (schedule->hour != local_tm->tm_hour || schedule->minute != local_tm->tm_min) {
        return false;
    }

    if (schedule->recurrence == APP_SCHEDULE_RECURRENCE_DAILY) {
        return true;
    }

    if (schedule->recurrence == APP_SCHEDULE_RECURRENCE_WEEKLY) {
        return (schedule->days_of_week_mask & (1u << local_tm->tm_wday)) != 0;
    }

    if (!parse_one_shot_date(schedule->one_shot_date, &year, &month, &day)) {
        return false;
    }

    return year == (local_tm->tm_year + 1900) &&
        month == (local_tm->tm_mon + 1) &&
        day == local_tm->tm_mday;
}

static void fill_schedule_command(
    const app_schedule_store_t *store,
    const app_schedule_t *schedule,
    uint64_t minute_key,
    time_t utc_now,
    app_command_t *command
) {
    char minute_key_text[24] = {0};
    size_t fixed_len = strlen("sch--");
    size_t available_for_schedule = 0;

    memset(command, 0, sizeof(*command));
    snprintf(minute_key_text, sizeof(minute_key_text), "%llu", (unsigned long long)minute_key);
    available_for_schedule = (sizeof(command->id) - 1) > (fixed_len + strlen(minute_key_text))
        ? (sizeof(command->id) - 1) - fixed_len - strlen(minute_key_text)
        : 0;
    snprintf(
        command->id,
        sizeof(command->id),
        "sch-%.*s-%s",
        (int)available_for_schedule,
        schedule->id,
        minute_key_text
    );
    strncpy(
        command->device_id,
        store->device_id[0] != '\0' ? store->device_id : schedule->device_id,
        sizeof(command->device_id) - 1
    );
    command->source = APP_COMMAND_SOURCE_INTERNAL;
    command->timestamp = utc_now;

    switch (schedule->type) {
        case APP_SCHEDULE_POWER_ON:
            command->type = APP_COMMAND_POWER_ON;
            break;
        case APP_SCHEDULE_POWER_OFF:
            command->type = APP_COMMAND_POWER_OFF;
            break;
        default:
            command->type = APP_COMMAND_RUN_DRAIN;
            strncpy(command->reason, "schedule", sizeof(command->reason) - 1);
            break;
    }
}

static void preserve_runtime_fields(app_schedule_store_t *next_store) {
    size_t next_index = 0;
    size_t current_index = 0;

    for (next_index = 0; next_index < next_store->count; ++next_index) {
        for (current_index = 0; current_index < s_store.count; ++current_index) {
            if (strcmp(next_store->items[next_index].id, s_store.items[current_index].id) == 0) {
                next_store->items[next_index].last_trigger_key = s_store.items[current_index].last_trigger_key;
                next_store->items[next_index].last_triggered_at_utc = s_store.items[current_index].last_triggered_at_utc;
                break;
            }
        }
    }
}

static void log_schedule_summary(const app_schedule_t *schedule) {
    ESP_LOGI(
        LOG_TAG_PROTOCOL,
        "Schedule salvo: id=%s type=%d recurrence=%d enabled=%d time=%02u:%02u mask=0x%02X oneShot=%s",
        schedule->id,
        schedule->type,
        schedule->recurrence,
        schedule->enabled,
        schedule->hour,
        schedule->minute,
        schedule->days_of_week_mask,
        schedule->one_shot_date[0] != '\0' ? schedule->one_shot_date : "-"
    );
}

static void scheduler_task(void *arg) {
    (void)arg;

    while (true) {
        struct tm local_tm = {0};
        time_t utc_now = 0;
        uint64_t minute_key = 0;
        size_t index = 0;

        if (!time_sync_is_time_valid()) {
            if (!s_waiting_for_time_logged) {
                ESP_LOGW(
                    LOG_TAG_PROTOCOL,
                    "Scheduler aguardando hora valida via SNTP antes de executar rotinas"
                );
                s_waiting_for_time_logged = true;
            }

            vTaskDelay(pdMS_TO_TICKS(APP_SCHEDULE_POLL_INTERVAL_MS));
            continue;
        }

        if (s_waiting_for_time_logged) {
            ESP_LOGI(LOG_TAG_PROTOCOL, "Hora valida detectada; scheduler habilitado");
            s_waiting_for_time_logged = false;
        }

        if (!time_sync_get_local_time(&local_tm, &utc_now)) {
            vTaskDelay(pdMS_TO_TICKS(APP_SCHEDULE_POLL_INTERVAL_MS));
            continue;
        }

        minute_key = build_minute_key(&local_tm);

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
            bool lock_held = true;

            for (index = 0; index < s_store.count; ++index) {
                app_schedule_t *schedule = &s_store.items[index];
                app_schedule_t schedule_copy = {0};
                app_command_t command = {0};
                command_result_t result = {0};
                esp_err_t err = ESP_OK;
                bool should_publish_schedules = false;
                char now_text[APP_TIMESTAMP_MAX_LEN + 1] = {0};

                if (schedule->last_trigger_key == minute_key) {
                    continue;
                }

                if (!schedule_matches_now(schedule, &local_tm)) {
                    continue;
                }

                schedule->last_trigger_key = minute_key;
                schedule->last_triggered_at_utc = utc_now;
                should_publish_schedules = schedule->recurrence == APP_SCHEDULE_RECURRENCE_ONE_SHOT;

                if (should_publish_schedules) {
                    schedule->enabled = false;
                    format_utc_timestamp(utc_now, now_text, sizeof(now_text));
                    strncpy(schedule->updated_at, now_text, sizeof(schedule->updated_at) - 1);
                }

                (void)save_store_locked();
                schedule_copy = *schedule;
                fill_schedule_command(&s_store, schedule, minute_key, utc_now, &command);

                ESP_LOGI(
                    LOG_TAG_PROTOCOL,
                    "Schedule elegivel para execucao: id=%s command=%s local=%04d-%02d-%02d %02d:%02d",
                    schedule_copy.id,
                    command.type == APP_COMMAND_POWER_ON
                        ? "power-on"
                        : (command.type == APP_COMMAND_POWER_OFF ? "power-off" : "run-drain"),
                    local_tm.tm_year + 1900,
                    local_tm.tm_mon + 1,
                    local_tm.tm_mday,
                    local_tm.tm_hour,
                    local_tm.tm_min
                );

                xSemaphoreGive(s_lock);
                lock_held = false;

                err = ihm_mqtt_adapter_execute_command(&command, &result);

                ESP_LOGI(
                    LOG_TAG_PROTOCOL,
                    "Schedule executado: id=%s accepted=%d applied=%d status=%d code=%s err=%s",
                    schedule_copy.id,
                    result.accepted,
                    result.applied,
                    result.status,
                    result.code[0] != '\0' ? result.code : "ok",
                    esp_err_to_name(err)
                );

                ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_state());

                if (should_publish_schedules) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(mqtt_manager_publish_schedules());
                }

                if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
                    break;
                }

                lock_held = true;
            }

            if (lock_held) {
                xSemaphoreGive(s_lock);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(APP_SCHEDULE_POLL_INTERVAL_MS));
    }
}

esp_err_t schedule_manager_restore(void) {
    esp_err_t err = ESP_OK;

    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = load_store_locked();
    if (err == ESP_OK) {
        time_sync_set_offset_minutes(s_store.timezone_offset_minutes);
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t schedule_manager_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(time_sync_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(schedule_manager_restore());
    xTaskCreate(
        scheduler_task,
        "schedule_task",
        APP_SCHEDULE_TASK_STACK_SIZE,
        NULL,
        APP_SCHEDULE_TASK_PRIORITY,
        NULL
    );

    ESP_LOGI(
        LOG_TAG_PROTOCOL,
        "Scheduler inicializado: %u rotinas carregadas, offset=%d min",
        (unsigned int)s_store.count,
        s_store.timezone_offset_minutes
    );

    s_initialized = true;
    return ESP_OK;
}

esp_err_t schedule_manager_handle_mqtt_payload(
    const char *payload,
    char *error_code,
    size_t error_code_len,
    char *error_message,
    size_t error_message_len
) {
    app_schedule_store_t parsed = {0};
    esp_err_t err = protocol_json_parse_schedules_payload(payload, &parsed);
    char payload_timestamp[APP_TIMESTAMP_MAX_LEN + 1] = {0};
    size_t index = 0;

    if (error_code != NULL && error_code_len > 0) {
        error_code[0] = '\0';
    }

    if (error_message != NULL && error_message_len > 0) {
        error_message[0] = '\0';
    }

    if (err != ESP_OK) {
        if (error_code != NULL && error_code_len > 0) {
            strncpy(error_code, "schedule_parse_failed", error_code_len - 1);
        }
        if (error_message != NULL && error_message_len > 0) {
            strncpy(error_message, "Payload de schedules invalido", error_message_len - 1);
        }
        return err;
    }

    if (parsed.timezone_offset_minutes < -720 || parsed.timezone_offset_minutes > 840) {
        parsed.timezone_offset_minutes = APP_SCHEDULE_TZ_OFFSET_MIN_DEFAULT;
    }

    if (protocol_json_extract_timestamp(payload, payload_timestamp, sizeof(payload_timestamp))) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(time_sync_set_utc_time_from_iso8601(payload_timestamp));
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        if (error_code != NULL && error_code_len > 0) {
            strncpy(error_code, "schedule_lock_timeout", error_code_len - 1);
        }
        if (error_message != NULL && error_message_len > 0) {
            strncpy(error_message, "Nao foi possivel salvar os schedules agora", error_message_len - 1);
        }
        return ESP_ERR_TIMEOUT;
    }

    preserve_runtime_fields(&parsed);
    s_store = parsed;
    err = save_store_locked();
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        if (error_code != NULL && error_code_len > 0) {
            strncpy(error_code, "schedule_persist_failed", error_code_len - 1);
        }
        if (error_message != NULL && error_message_len > 0) {
            strncpy(error_message, "Falha ao persistir schedules na NVS", error_message_len - 1);
        }
        return err;
    }

    time_sync_set_offset_minutes(s_store.timezone_offset_minutes);

    ESP_LOGI(
        LOG_TAG_PROTOCOL,
        "Schedules recebidos e salvos: count=%u revision=%s tz=%s offset=%d",
        (unsigned int)s_store.count,
        s_store.revision[0] != '\0' ? s_store.revision : "sem-revision",
        s_store.timezone_name[0] != '\0' ? s_store.timezone_name : "nao informado",
        s_store.timezone_offset_minutes
    );

    for (index = 0; index < s_store.count; ++index) {
        log_schedule_summary(&s_store.items[index]);
    }

    return ESP_OK;
}

esp_err_t schedule_manager_build_payload(const char *device_id, char **out_json) {
    app_schedule_store_t snapshot = {0};

    if (out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    snapshot = s_store;
    xSemaphoreGive(s_lock);

    if (snapshot.device_id[0] == '\0' && device_id != NULL) {
        strncpy(snapshot.device_id, device_id, sizeof(snapshot.device_id) - 1);
    }

    return protocol_json_build_schedules_payload(
        device_id != NULL ? device_id : snapshot.device_id,
        &snapshot,
        out_json
    );
}

esp_err_t schedule_manager_get_store(app_schedule_store_t *store) {
    if (store == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *store = s_store;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool schedule_manager_has_schedules(void) {
    bool has_schedules = false;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    has_schedules = s_store.count > 0;
    xSemaphoreGive(s_lock);
    return has_schedules;
}
