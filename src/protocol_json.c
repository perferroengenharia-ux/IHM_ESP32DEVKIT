#include "protocol_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static void cp(char *dst, size_t len, const char *src) {
    if (dst == NULL || len == 0) {
        return;
    }

    dst[0] = '\0';

    if (src != NULL) {
        strncpy(dst, src, len - 1);
    }
}

static void ts(time_t value, char *buffer, size_t len) {
    struct tm tm_value = {0};
    time_t safe_value = value > 0 ? value : time(NULL);

    gmtime_r(&safe_value, &tm_value);
    strftime(buffer, len, "%Y-%m-%dT%H:%M:%SZ", &tm_value);
}

static esp_err_t out(cJSON *root_object, char **out_json) {
    char *json = NULL;

    if (root_object == NULL || out_json == NULL) {
        cJSON_Delete(root_object);
        return ESP_ERR_INVALID_ARG;
    }

    json = cJSON_PrintUnformatted(root_object);
    cJSON_Delete(root_object);

    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = json;
    return ESP_OK;
}

static cJSON *root(const char *device_id) {
    char timestamp[APP_TIMESTAMP_MAX_LEN + 1] = {0};
    cJSON *root_object = cJSON_CreateObject();

    if (root_object == NULL) {
        return NULL;
    }

    ts(time(NULL), timestamp, sizeof(timestamp));
    cJSON_AddStringToObject(root_object, "schema", APP_PROTOCOL_SCHEMA_VERSION);
    cJSON_AddStringToObject(root_object, "deviceId", device_id != NULL ? device_id : "");
    cJSON_AddStringToObject(root_object, "timestamp", timestamp);
    cJSON_AddStringToObject(root_object, "source", "ihm");

    return root_object;
}

static cJSON *build_error_object(
    const char *device_id,
    const char *error_id,
    const char *code,
    const char *message,
    bool recoverable
) {
    char created_at[APP_TIMESTAMP_MAX_LEN + 1] = {0};
    cJSON *error_object = cJSON_CreateObject();

    if (error_object == NULL) {
        return NULL;
    }

    ts(time(NULL), created_at, sizeof(created_at));
    cJSON_AddStringToObject(error_object, "id", (error_id != NULL && error_id[0] != '\0') ? error_id : "ihm-error");
    cJSON_AddStringToObject(error_object, "deviceId", device_id != NULL ? device_id : "");
    cJSON_AddStringToObject(error_object, "code", code != NULL ? code : "unknown");
    cJSON_AddStringToObject(error_object, "message", message != NULL ? message : "Erro nao especificado");
    cJSON_AddStringToObject(error_object, "createdAt", created_at);
    cJSON_AddBoolToObject(error_object, "recoverable", recoverable);

    return error_object;
}

static const char *cm(app_connection_mode_t mode) {
    switch (mode) {
        case APP_CONNECTION_MODE_CLOUD:
            return "cloud";
        case APP_CONNECTION_MODE_LOCAL_LAN:
            return "local-lan";
        case APP_CONNECTION_MODE_LOCAL_AP:
            return "local-ap";
        default:
            return "simulation";
    }
}

static const char *rs(app_ready_state_t state) {
    switch (state) {
        case APP_READY_STATE_READY:
            return "ready";
        case APP_READY_STATE_STARTING:
            return "starting";
        case APP_READY_STATE_RUNNING:
            return "running";
        case APP_READY_STATE_STOPPING:
            return "stopping";
        case APP_READY_STATE_DRAINING:
            return "draining";
        case APP_READY_STATE_FAULT:
            return "fault";
        default:
            return "offline";
    }
}

static const char *ps(app_peripheral_state_t state) {
    switch (state) {
        case APP_PERIPHERAL_ON:
            return "on";
        case APP_PERIPHERAL_OFF:
            return "off";
        case APP_PERIPHERAL_UNAVAILABLE:
            return "unavailable";
        default:
            return "unknown";
    }
}

static const char *ws(app_water_level_state_t state) {
    switch (state) {
        case APP_WATER_LEVEL_OK:
            return "ok";
        case APP_WATER_LEVEL_LOW:
            return "low";
        case APP_WATER_LEVEL_DISABLED:
            return "disabled";
        default:
            return "unknown";
    }
}

static const char *cs(app_last_command_status_t status) {
    switch (status) {
        case APP_LAST_COMMAND_IDLE:
            return "idle";
        case APP_LAST_COMMAND_SENDING:
            return "sending";
        case APP_LAST_COMMAND_APPLIED:
            return "applied";
        default:
            return "failed";
    }
}

static const char *dm(app_drain_mode_t mode) {
    switch (mode) {
        case APP_DRAIN_MODE_TIMED:
            return "timed";
        case APP_DRAIN_MODE_UNTIL_SENSOR:
            return "until-sensor";
        case APP_DRAIN_MODE_HYBRID:
            return "hybrid";
        default:
            return "disabled";
    }
}

static const char *pm(app_pump_logic_mode_t mode) {
    switch (mode) {
        case APP_PUMP_LOGIC_LINKED:
            return "linked";
        case APP_PUMP_LOGIC_INDEPENDENT:
            return "independent";
        case APP_PUMP_LOGIC_FORCED_ON:
            return "forced-on";
        default:
            return "forced-off";
    }
}

static const char *wm(app_water_sensor_mode_t mode) {
    switch (mode) {
        case APP_WATER_SENSOR_MODE_NORMAL:
            return "normal";
        case APP_WATER_SENSOR_MODE_INVERTED:
            return "inverted";
        default:
            return "disabled";
    }
}

static const char *rm(app_resume_mode_t mode) {
    switch (mode) {
        case APP_RESUME_LAST_STATE:
            return "resume-last-state";
        case APP_RESUME_ALWAYS_OFF:
            return "always-off";
        default:
            return "always-on";
    }
}

static const char *schedule_type_to_string(app_schedule_type_t type) {
    switch (type) {
        case APP_SCHEDULE_POWER_ON:
            return "power-on";
        case APP_SCHEDULE_POWER_OFF:
            return "power-off";
        default:
            return "drain-cycle";
    }
}

static const char *schedule_recurrence_to_string(app_schedule_recurrence_t recurrence) {
    switch (recurrence) {
        case APP_SCHEDULE_RECURRENCE_ONE_SHOT:
            return "one-shot";
        case APP_SCHEDULE_RECURRENCE_DAILY:
            return "daily";
        default:
            return "weekly";
    }
}

const char *protocol_json_command_type_to_string(app_command_type_t type) {
    switch (type) {
        case APP_COMMAND_POWER_ON:
            return "power-on";
        case APP_COMMAND_POWER_OFF:
            return "power-off";
        case APP_COMMAND_SET_FREQUENCY:
            return "set-frequency";
        case APP_COMMAND_SET_PUMP:
            return "set-pump";
        case APP_COMMAND_SET_SWING:
            return "set-swing";
        case APP_COMMAND_RUN_DRAIN:
            return "run-drain";
        case APP_COMMAND_STOP_DRAIN:
            return "stop-drain";
        case APP_COMMAND_REQUEST_STATUS:
            return "request-status";
        default:
            return "request-capabilities";
    }
}

static bool pct(const char *type_text, app_command_type_t *out_type) {
    if (type_text == NULL || out_type == NULL) {
        return false;
    }

    if (strcmp(type_text, "power-on") == 0) {
        *out_type = APP_COMMAND_POWER_ON;
        return true;
    }

    if (strcmp(type_text, "power-off") == 0) {
        *out_type = APP_COMMAND_POWER_OFF;
        return true;
    }

    if (strcmp(type_text, "set-frequency") == 0) {
        *out_type = APP_COMMAND_SET_FREQUENCY;
        return true;
    }

    if (strcmp(type_text, "set-pump") == 0) {
        *out_type = APP_COMMAND_SET_PUMP;
        return true;
    }

    if (strcmp(type_text, "set-swing") == 0) {
        *out_type = APP_COMMAND_SET_SWING;
        return true;
    }

    if (strcmp(type_text, "run-drain") == 0 || strcmp(type_text, "start-drain") == 0) {
        *out_type = APP_COMMAND_RUN_DRAIN;
        return true;
    }

    if (strcmp(type_text, "stop-drain") == 0) {
        *out_type = APP_COMMAND_STOP_DRAIN;
        return true;
    }

    if (strcmp(type_text, "request-status") == 0 || strcmp(type_text, "get-status") == 0) {
        *out_type = APP_COMMAND_REQUEST_STATUS;
        return true;
    }

    if (
        strcmp(type_text, "request-capabilities") == 0 ||
        strcmp(type_text, "get-capabilities") == 0
    ) {
        *out_type = APP_COMMAND_REQUEST_CAPABILITIES;
        return true;
    }

    return false;
}

static bool parse_schedule_type(const char *type_text, app_schedule_type_t *out_type) {
    if (type_text == NULL || out_type == NULL) {
        return false;
    }

    if (strcmp(type_text, "power-on") == 0) {
        *out_type = APP_SCHEDULE_POWER_ON;
        return true;
    }

    if (strcmp(type_text, "power-off") == 0) {
        *out_type = APP_SCHEDULE_POWER_OFF;
        return true;
    }

    if (strcmp(type_text, "drain-cycle") == 0) {
        *out_type = APP_SCHEDULE_DRAIN_CYCLE;
        return true;
    }

    return false;
}

static bool parse_schedule_recurrence(const char *recurrence_text, app_schedule_recurrence_t *out_recurrence) {
    if (recurrence_text == NULL || out_recurrence == NULL) {
        return false;
    }

    if (strcmp(recurrence_text, "one-shot") == 0) {
        *out_recurrence = APP_SCHEDULE_RECURRENCE_ONE_SHOT;
        return true;
    }

    if (strcmp(recurrence_text, "daily") == 0) {
        *out_recurrence = APP_SCHEDULE_RECURRENCE_DAILY;
        return true;
    }

    if (strcmp(recurrence_text, "weekly") == 0) {
        *out_recurrence = APP_SCHEDULE_RECURRENCE_WEEKLY;
        return true;
    }

    return false;
}

static bool parse_hhmm(const char *time_text, uint8_t *out_hour, uint8_t *out_minute) {
    unsigned int hour = 0;
    unsigned int minute = 0;

    if (time_text == NULL || out_hour == NULL || out_minute == NULL) {
        return false;
    }

    if (sscanf(time_text, "%2u:%2u", &hour, &minute) != 2) {
        return false;
    }

    if (hour > 23 || minute > 59) {
        return false;
    }

    *out_hour = (uint8_t)hour;
    *out_minute = (uint8_t)minute;
    return true;
}

static bool parse_days_mask(cJSON *days_array, uint8_t *out_mask) {
    cJSON *day = NULL;
    uint8_t mask = 0;

    if (!cJSON_IsArray(days_array) || out_mask == NULL) {
        return false;
    }

    cJSON_ArrayForEach(day, days_array) {
        if (!cJSON_IsNumber(day) || day->valueint < 0 || day->valueint > 6) {
            return false;
        }

        mask |= (uint8_t)(1u << day->valueint);
    }

    *out_mask = mask;
    return true;
}

esp_err_t protocol_json_parse_command(const char *json, app_command_t *command) {
    cJSON *root_object = NULL;
    cJSON *message = NULL;
    cJSON *payload = NULL;
    cJSON *item = NULL;

    if (json == NULL || command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(command, 0, sizeof(*command));
    root_object = cJSON_Parse(json);

    if (root_object == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    message = cJSON_GetObjectItemCaseSensitive(root_object, "command");

    if (!cJSON_IsObject(message)) {
        message = root_object;
    }

    item = cJSON_GetObjectItemCaseSensitive(message, "id");
    if (cJSON_IsString(item)) {
        cp(command->id, sizeof(command->id), item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive(message, "deviceId");
    if (cJSON_IsString(item)) {
        cp(command->device_id, sizeof(command->device_id), item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive(message, "type");
    if (!cJSON_IsString(item) || !pct(item->valuestring, &command->type)) {
        cJSON_Delete(root_object);
        return ESP_ERR_INVALID_ARG;
    }

    payload = cJSON_GetObjectItemCaseSensitive(message, "payload");
    if (!cJSON_IsObject(payload)) {
        payload = NULL;
    }

    if (command->type == APP_COMMAND_SET_FREQUENCY) {
        item = cJSON_GetObjectItemCaseSensitive(payload, "freqTargetHz");
        if (!cJSON_IsNumber(item)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }
        command->freq_target_hz = item->valueint;
    } else if (
        command->type == APP_COMMAND_SET_PUMP ||
        command->type == APP_COMMAND_SET_SWING
    ) {
        item = cJSON_GetObjectItemCaseSensitive(payload, "enabled");
        if (!cJSON_IsBool(item)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }
        command->enabled = cJSON_IsTrue(item);
    } else if (
        command->type == APP_COMMAND_RUN_DRAIN ||
        command->type == APP_COMMAND_STOP_DRAIN
    ) {
        item = cJSON_GetObjectItemCaseSensitive(payload, "reason");
        if (cJSON_IsString(item)) {
            cp(command->reason, sizeof(command->reason), item->valuestring);
        }
    }

    command->source = APP_COMMAND_SOURCE_MQTT;
    command->timestamp = time(NULL);
    cJSON_Delete(root_object);

    return ESP_OK;
}

esp_err_t protocol_json_parse_schedules_payload(const char *json, app_schedule_store_t *store) {
    cJSON *root_object = NULL;
    cJSON *schedules_array = NULL;
    cJSON *item = NULL;
    size_t index = 0;

    if (json == NULL || store == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(store, 0, sizeof(*store));
    store->timezone_offset_minutes = APP_SCHEDULE_TZ_OFFSET_MIN_DEFAULT;
    root_object = cJSON_Parse(json);

    if (root_object == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    item = cJSON_GetObjectItemCaseSensitive(root_object, "deviceId");
    if (cJSON_IsString(item)) {
        cp(store->device_id, sizeof(store->device_id), item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive(root_object, "revision");
    if (cJSON_IsString(item)) {
        cp(store->revision, sizeof(store->revision), item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive(root_object, "timezone");
    if (cJSON_IsString(item)) {
        cp(store->timezone_name, sizeof(store->timezone_name), item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive(root_object, "timezoneOffsetMinutes");
    if (cJSON_IsNumber(item)) {
        store->timezone_offset_minutes = item->valueint;
    }

    schedules_array = cJSON_GetObjectItemCaseSensitive(root_object, "schedules");
    if (!cJSON_IsArray(schedules_array)) {
        cJSON_Delete(root_object);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_ArrayForEach(item, schedules_array) {
        cJSON *field = NULL;
        app_schedule_t *schedule = NULL;
        char time_text[6] = {0};
        uint8_t days_mask = 0;

        if (index >= APP_SCHEDULE_MAX_COUNT) {
            cJSON_Delete(root_object);
            return ESP_ERR_NO_MEM;
        }

        if (!cJSON_IsObject(item)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }

        schedule = &store->items[index];

        field = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!cJSON_IsString(field)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }
        cp(schedule->id, sizeof(schedule->id), field->valuestring);

        field = cJSON_GetObjectItemCaseSensitive(item, "deviceId");
        if (store->device_id[0] != '\0') {
            cp(schedule->device_id, sizeof(schedule->device_id), store->device_id);
        } else if (cJSON_IsString(field)) {
            cp(schedule->device_id, sizeof(schedule->device_id), field->valuestring);
        } else {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }

        field = cJSON_GetObjectItemCaseSensitive(item, "type");
        if (!cJSON_IsString(field) || !parse_schedule_type(field->valuestring, &schedule->type)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }

        field = cJSON_GetObjectItemCaseSensitive(item, "recurrence");
        if (!cJSON_IsString(field) || !parse_schedule_recurrence(field->valuestring, &schedule->recurrence)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }

        field = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        if (!cJSON_IsBool(field)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }
        schedule->enabled = cJSON_IsTrue(field);

        field = cJSON_GetObjectItemCaseSensitive(item, "time");
        if (!cJSON_IsString(field) || !parse_hhmm(field->valuestring, &schedule->hour, &schedule->minute)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }
        cp(time_text, sizeof(time_text), field->valuestring);

        field = cJSON_GetObjectItemCaseSensitive(item, "daysOfWeek");
        if (!parse_days_mask(field, &days_mask)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }
        schedule->days_of_week_mask = days_mask;

        field = cJSON_GetObjectItemCaseSensitive(item, "oneShotDate");
        if (cJSON_IsString(field)) {
            cp(schedule->one_shot_date, sizeof(schedule->one_shot_date), field->valuestring);
        } else {
            schedule->one_shot_date[0] = '\0';
        }

        field = cJSON_GetObjectItemCaseSensitive(item, "createdAt");
        if (!cJSON_IsString(field)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }
        cp(schedule->created_at, sizeof(schedule->created_at), field->valuestring);

        field = cJSON_GetObjectItemCaseSensitive(item, "updatedAt");
        if (!cJSON_IsString(field)) {
            cJSON_Delete(root_object);
            return ESP_ERR_INVALID_ARG;
        }
        cp(schedule->updated_at, sizeof(schedule->updated_at), field->valuestring);

        index++;
    }

    store->count = index;
    cJSON_Delete(root_object);

    return ESP_OK;
}

bool protocol_json_source_equals(const char *json, const char *expected_source) {
    cJSON *root_object = NULL;
    cJSON *source_item = NULL;
    bool matches = false;

    if (json == NULL || expected_source == NULL) {
        return false;
    }

    root_object = cJSON_Parse(json);
    if (root_object == NULL) {
        return false;
    }

    source_item = cJSON_GetObjectItemCaseSensitive(root_object, "source");
    matches = cJSON_IsString(source_item) && strcmp(source_item->valuestring, expected_source) == 0;
    cJSON_Delete(root_object);

    return matches;
}

bool protocol_json_extract_timestamp(const char *json, char *out_timestamp, size_t out_timestamp_len) {
    cJSON *root_object = NULL;
    cJSON *timestamp_item = NULL;
    bool found = false;

    if (json == NULL || out_timestamp == NULL || out_timestamp_len == 0) {
        return false;
    }

    out_timestamp[0] = '\0';
    root_object = cJSON_Parse(json);
    if (root_object == NULL) {
        return false;
    }

    timestamp_item = cJSON_GetObjectItemCaseSensitive(root_object, "timestamp");
    if (cJSON_IsString(timestamp_item)) {
        cp(out_timestamp, out_timestamp_len, timestamp_item->valuestring);
        found = true;
    }

    cJSON_Delete(root_object);
    return found;
}

static cJSON *status_obj(const device_state_t *state) {
    char last_seen[APP_TIMESTAMP_MAX_LEN + 1] = {0};
    cJSON *object = cJSON_CreateObject();

    ts(state->last_seen, last_seen, sizeof(last_seen));
    cJSON_AddBoolToObject(object, "deviceOnline", state->device_online);
    cJSON_AddStringToObject(object, "connectionMode", cm(state->connection_mode));
    cJSON_AddStringToObject(object, "lastSeen", last_seen);
    cJSON_AddStringToObject(object, "readyState", rs(state->ready_state));

    return object;
}

static cJSON *state_obj(const device_state_t *state) {
    cJSON *object = status_obj(state);

    cJSON_AddBoolToObject(object, "inverterRunning", state->inverter_running);
    cJSON_AddNumberToObject(object, "freqCurrentHz", state->freq_current_hz);
    cJSON_AddNumberToObject(object, "freqTargetHz", state->freq_target_hz);
    cJSON_AddStringToObject(object, "pumpState", ps(state->pump_state));
    cJSON_AddStringToObject(object, "swingState", ps(state->swing_state));
    cJSON_AddStringToObject(object, "drainState", ps(state->drain_state));
    cJSON_AddStringToObject(object, "waterLevelState", ws(state->water_level_state));
    cJSON_AddStringToObject(object, "lastCommandStatus", cs(state->last_command_status));

    if (state->last_error_code[0] != '\0') {
        cJSON_AddStringToObject(object, "lastErrorCode", state->last_error_code);
    } else {
        cJSON_AddNullToObject(object, "lastErrorCode");
    }

    return object;
}

esp_err_t protocol_json_build_status(const char *device_id, const device_state_t *state, char **out_json) {
    cJSON *root_object = NULL;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root_object = root(device_id);
    cJSON_AddItemToObject(root_object, "status", status_obj(state));
    return out(root_object, out_json);
}

esp_err_t protocol_json_build_state(const char *device_id, const device_state_t *state, char **out_json) {
    cJSON *root_object = NULL;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root_object = root(device_id);
    cJSON_AddItemToObject(root_object, "state", state_obj(state));
    return out(root_object, out_json);
}

esp_err_t protocol_json_build_capabilities(
    const char *device_id,
    const device_capabilities_t *capabilities,
    char **out_json
) {
    cJSON *root_object = NULL;
    cJSON *object = NULL;

    if (capabilities == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root_object = root(device_id);
    object = cJSON_CreateObject();
    cJSON_AddNumberToObject(object, "fMinHz", capabilities->f_min_hz);
    cJSON_AddNumberToObject(object, "fMaxHz", capabilities->f_max_hz);
    cJSON_AddBoolToObject(object, "pumpAvailable", capabilities->pump_available);
    cJSON_AddBoolToObject(object, "swingAvailable", capabilities->swing_available);
    cJSON_AddBoolToObject(object, "drainAvailable", capabilities->drain_available);
    cJSON_AddBoolToObject(object, "waterSensorEnabled", capabilities->water_sensor_enabled);
    cJSON_AddStringToObject(object, "drainMode", dm(capabilities->drain_mode));
    cJSON_AddNumberToObject(object, "drainTimeSec", capabilities->drain_time_sec);
    cJSON_AddNumberToObject(object, "drainReturnDelaySec", capabilities->drain_return_delay_sec);
    cJSON_AddStringToObject(object, "pumpLogicMode", pm(capabilities->pump_logic_mode));
    cJSON_AddStringToObject(object, "waterSensorMode", wm(capabilities->water_sensor_mode));
    cJSON_AddNumberToObject(object, "preWetSec", capabilities->pre_wet_sec);
    cJSON_AddNumberToObject(object, "dryPanelSec", capabilities->dry_panel_sec);
    cJSON_AddNumberToObject(object, "dryPanelFreqHz", capabilities->dry_panel_freq_hz);
    cJSON_AddStringToObject(object, "resumeMode", rm(capabilities->resume_mode));
    cJSON_AddStringToObject(
        object,
        "autoResetMode",
        capabilities->auto_reset_mode == APP_AUTO_RESET_ENABLED ? "enabled" : "disabled"
    );
    cJSON_AddItemToObject(root_object, "capabilities", object);

    return out(root_object, out_json);
}

esp_err_t protocol_json_build_schedules_payload(
    const char *device_id,
    const app_schedule_store_t *store,
    char **out_json
) {
    cJSON *root_object = NULL;
    cJSON *schedules_array = NULL;
    size_t index = 0;

    if (store == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root_object = root(device_id != NULL ? device_id : store->device_id);
    schedules_array = cJSON_CreateArray();

    if (store->revision[0] != '\0') {
        cJSON_AddStringToObject(root_object, "revision", store->revision);
    }

    if (store->timezone_name[0] != '\0') {
        cJSON_AddStringToObject(root_object, "timezone", store->timezone_name);
    }

    cJSON_AddNumberToObject(root_object, "timezoneOffsetMinutes", store->timezone_offset_minutes);

    for (index = 0; index < store->count && index < APP_SCHEDULE_MAX_COUNT; ++index) {
        const app_schedule_t *schedule = &store->items[index];
        char time_text[8] = {0};
        cJSON *schedule_object = cJSON_CreateObject();
        int weekday = 0;

        snprintf(time_text, sizeof(time_text), "%02u:%02u", schedule->hour, schedule->minute);
        cJSON_AddStringToObject(schedule_object, "id", schedule->id);
        cJSON_AddStringToObject(
            schedule_object,
            "deviceId",
            store->device_id[0] != '\0' ? store->device_id : schedule->device_id
        );
        cJSON_AddStringToObject(schedule_object, "type", schedule_type_to_string(schedule->type));
        cJSON_AddStringToObject(
            schedule_object,
            "recurrence",
            schedule_recurrence_to_string(schedule->recurrence)
        );
        cJSON_AddBoolToObject(schedule_object, "enabled", schedule->enabled);
        cJSON_AddStringToObject(schedule_object, "time", time_text);

        cJSON *days_array = cJSON_AddArrayToObject(schedule_object, "daysOfWeek");
        for (weekday = 0; weekday <= 6; ++weekday) {
            if ((schedule->days_of_week_mask & (1u << weekday)) != 0) {
                cJSON_AddItemToArray(days_array, cJSON_CreateNumber(weekday));
            }
        }

        if (schedule->one_shot_date[0] != '\0') {
            cJSON_AddStringToObject(schedule_object, "oneShotDate", schedule->one_shot_date);
        } else {
            cJSON_AddNullToObject(schedule_object, "oneShotDate");
        }

        cJSON_AddStringToObject(schedule_object, "createdAt", schedule->created_at);
        cJSON_AddStringToObject(schedule_object, "updatedAt", schedule->updated_at);
        cJSON_AddItemToArray(schedules_array, schedule_object);
    }

    cJSON_AddItemToObject(root_object, "schedules", schedules_array);
    return out(root_object, out_json);
}

esp_err_t protocol_json_build_command_ack(
    const char *device_id,
    const app_command_t *command,
    const command_result_t *result,
    const device_state_t *state,
    char **out_json
) {
    cJSON *root_object = NULL;
    cJSON *error_object = NULL;

    if (command == NULL || result == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root_object = root(device_id);
    cJSON_AddStringToObject(root_object, "id", command->id);
    cJSON_AddStringToObject(root_object, "type", protocol_json_command_type_to_string(command->type));
    cJSON_AddBoolToObject(root_object, "accepted", result->accepted);
    cJSON_AddBoolToObject(root_object, "applied", result->applied);
    cJSON_AddStringToObject(root_object, "status", cs(result->status));
    cJSON_AddItemToObject(root_object, "state", state_obj(state));

    if (result->status == APP_LAST_COMMAND_FAILED) {
        error_object = build_error_object(device_id, command->id, result->code, result->message, true);
        cJSON_AddItemToObject(root_object, "error", error_object);
    } else {
        cJSON_AddNullToObject(root_object, "error");
    }

    return out(root_object, out_json);
}

esp_err_t protocol_json_build_error(const char *device_id, const char *code, const char *message, char **out_json) {
    cJSON *root_object = root(device_id);
    cJSON *errors = cJSON_CreateArray();

    cJSON_AddItemToArray(errors, build_error_object(device_id, "mqtt-error", code, message, true));
    cJSON_AddItemToObject(root_object, "errors", errors);

    return out(root_object, out_json);
}
