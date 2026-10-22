#ifndef HOME_SCREEN_H
#define HOME_SCREEN_H

#include <stddef.h>

#include "battery_service.h"
#include "canvas.h"

#define HOME_SCREEN_ITEM_COUNT 3

typedef enum {
    HOME_DESTINATION_TODO,
    HOME_DESTINATION_PHOTOS,
    HOME_DESTINATION_POMODORO,
} home_destination_t;

typedef struct {
    size_t selected_index;
} home_screen_state_t;

void home_screen_init(home_screen_state_t *state);
void home_screen_move(home_screen_state_t *state, int direction);
home_destination_t home_screen_selected_destination(const home_screen_state_t *state);
void home_screen_render(canvas_t *canvas, const home_screen_state_t *state, const battery_status_t *battery);

#endif
