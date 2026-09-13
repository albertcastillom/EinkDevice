#ifndef INPUT_SERVICE_H
#define INPUT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    INPUT_EVENT_UP,
    INPUT_EVENT_SELECT,
    INPUT_EVENT_DOWN,
} input_event_t;

esp_err_t input_service_init(void);
bool input_service_wait(input_event_t *event, uint32_t timeout_ms);
const char *input_service_event_name(input_event_t event);

#endif
