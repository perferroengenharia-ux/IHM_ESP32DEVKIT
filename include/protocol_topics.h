#ifndef PROTOCOL_TOPICS_H
#define PROTOCOL_TOPICS_H
#include "esp_err.h"
#include "app_types.h"
typedef struct { char base[APP_TOPIC_PREFIX_MAX_LEN+APP_DEVICE_ID_MAX_LEN+3]; char status[APP_TOPIC_PREFIX_MAX_LEN+APP_DEVICE_ID_MAX_LEN+16]; char state[APP_TOPIC_PREFIX_MAX_LEN+APP_DEVICE_ID_MAX_LEN+16]; char capabilities[APP_TOPIC_PREFIX_MAX_LEN+APP_DEVICE_ID_MAX_LEN+24]; char commands[APP_TOPIC_PREFIX_MAX_LEN+APP_DEVICE_ID_MAX_LEN+20]; char events[APP_TOPIC_PREFIX_MAX_LEN+APP_DEVICE_ID_MAX_LEN+16]; char errors[APP_TOPIC_PREFIX_MAX_LEN+APP_DEVICE_ID_MAX_LEN+16]; char schedules[APP_TOPIC_PREFIX_MAX_LEN+APP_DEVICE_ID_MAX_LEN+20]; } protocol_topic_bundle_t;
esp_err_t protocol_topics_build(const app_mqtt_config_t *config, protocol_topic_bundle_t *topics);
#endif
