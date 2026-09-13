#ifndef DISPLAY_SERVICE_H
#define DISPLAY_SERVICE_H

#include "canvas.h"
#include "esp_err.h"

esp_err_t display_service_init(void);
canvas_t *display_service_canvas(void);
esp_err_t display_service_present_full(void);
esp_err_t display_service_present_partial(void);

#endif
