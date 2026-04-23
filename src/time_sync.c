#include "time_sync.h"

#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "app_config.h"
#include "log_tags.h"

static bool s_initialized;
static bool s_started;
static int s_offset_minutes = APP_SCHEDULE_TZ_OFFSET_MIN_DEFAULT;

static int64_t days_from_civil(int year, unsigned int month, unsigned int day) {
    year -= month <= 2 ? 1 : 0;

    {
        int era = (year >= 0 ? year : year - 399) / 400;
        unsigned int year_of_era = (unsigned int)(year - era * 400);
        unsigned int day_of_year = (153u * (month + (month > 2 ? (unsigned int)-3 : 9u)) + 2u) / 5u + day - 1u;
        unsigned int day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;

        return (int64_t)era * 146097LL + (int64_t)day_of_era - 719468LL;
    }
}

static bool parse_utc_iso8601(const char *timestamp, time_t *out_epoch) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int64_t epoch = 0;

    if (timestamp == NULL || out_epoch == NULL) {
        return false;
    }

    if (sscanf(timestamp, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }

    if (
        year < 2024 ||
        month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 59
    ) {
        return false;
    }

    epoch = days_from_civil(year, (unsigned int)month, (unsigned int)day);
    epoch = (epoch * 86400LL) + ((int64_t)hour * 3600LL) + ((int64_t)minute * 60LL) + (int64_t)second;

    *out_epoch = (time_t)epoch;
    return true;
}

static void on_time_sync(struct timeval *tv) {
    (void)tv;
    ESP_LOGI(LOG_TAG_PROTOCOL, "Hora SNTP sincronizada com sucesso");
}

esp_err_t time_sync_init(void) {
    s_offset_minutes = APP_SCHEDULE_TZ_OFFSET_MIN_DEFAULT;
    s_initialized = true;
    return ESP_OK;
}

void time_sync_notify_network_ready(void) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(APP_SNTP_SERVER_PRIMARY);

    if (!s_initialized) {
        (void)time_sync_init();
    }

    if (s_started) {
        return;
    }

    config.start = false;
    config.wait_for_sync = false;
    config.sync_cb = on_time_sync;

    if (esp_netif_sntp_init(&config) == ESP_OK) {
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
        esp_sntp_setservername(1, APP_SNTP_SERVER_SECONDARY);
#endif
        (void)esp_netif_sntp_start();
        s_started = true;
        ESP_LOGI(
            LOG_TAG_PROTOCOL,
            "SNTP iniciado: servidores=%s,%s",
            APP_SNTP_SERVER_PRIMARY,
            APP_SNTP_SERVER_SECONDARY
        );
    } else {
        ESP_LOGW(LOG_TAG_PROTOCOL, "Falha ao iniciar SNTP");
    }
}

bool time_sync_is_time_valid(void) {
    return time(NULL) >= APP_TIME_VALID_UNIX_THRESHOLD;
}

esp_err_t time_sync_set_utc_time_from_iso8601(const char *timestamp) {
    struct timeval now_value = {0};

    if (!parse_utc_iso8601(timestamp, &now_value.tv_sec)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (settimeofday(&now_value, NULL) != 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(LOG_TAG_PROTOCOL, "Hora UTC ajustada a partir do payload do app: %s", timestamp);
    return ESP_OK;
}

void time_sync_set_offset_minutes(int offset_minutes) {
    if (offset_minutes < -720 || offset_minutes > 840) {
        offset_minutes = APP_SCHEDULE_TZ_OFFSET_MIN_DEFAULT;
    }

    s_offset_minutes = offset_minutes;
}

int time_sync_get_offset_minutes(void) {
    return s_offset_minutes;
}

bool time_sync_get_local_time(struct tm *out_tm, time_t *out_utc_now) {
    time_t utc_now = time(NULL);
    time_t adjusted = 0;

    if (out_tm == NULL || !time_sync_is_time_valid()) {
        return false;
    }

    adjusted = utc_now + ((time_t)s_offset_minutes * 60);
    gmtime_r(&adjusted, out_tm);

    if (out_utc_now != NULL) {
        *out_utc_now = utc_now;
    }

    return true;
}
