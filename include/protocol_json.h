#ifndef PROTOCOL_JSON_H
#define PROTOCOL_JSON_H

#include "esp_err.h"
#include "app_types.h"

esp_err_t protocol_json_parse_command(const char *json, app_command_t *command);
esp_err_t protocol_json_parse_schedules_payload(const char *json, app_schedule_store_t *store);
bool protocol_json_source_equals(const char *json, const char *expected_source);
bool protocol_json_extract_timestamp(const char *json, char *out_timestamp, size_t out_timestamp_len);

esp_err_t protocol_json_build_status(const char *device_id, const device_state_t *state, char **out_json);
esp_err_t protocol_json_build_state(const char *device_id, const device_state_t *state, char **out_json);
esp_err_t protocol_json_build_capabilities(const char *device_id, const device_capabilities_t *capabilities, char **out_json);
esp_err_t protocol_json_build_schedules_payload(const char *device_id, const app_schedule_store_t *store, char **out_json);
esp_err_t protocol_json_build_command_ack(
    const char *device_id,
    const app_command_t *command,
    const command_result_t *result,
    const device_state_t *state,
    char **out_json
);
esp_err_t protocol_json_build_error(const char *device_id, const char *code, const char *message, char **out_json);

const char *protocol_json_command_type_to_string(app_command_type_t type);

#endif
