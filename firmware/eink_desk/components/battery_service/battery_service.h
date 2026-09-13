#ifndef BATTERY_SERVICE_H
#define BATTERY_SERVICE_H

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    int percentage;
    bool battery_present;
    bool charging;
    bool usb_connected;
} battery_status_t;

esp_err_t battery_service_init(void);
esp_err_t battery_service_read(battery_status_t *status);

#endif
