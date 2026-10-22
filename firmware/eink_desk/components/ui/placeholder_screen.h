#ifndef PLACEHOLDER_SCREEN_H
#define PLACEHOLDER_SCREEN_H

#include "battery_service.h"
#include "canvas.h"

void placeholder_screen_render(canvas_t *canvas, const char *title, const battery_status_t *battery);

#endif
