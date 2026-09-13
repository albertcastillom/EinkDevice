#ifndef TODO_SCREEN_H
#define TODO_SCREEN_H

#include <stdbool.h>
#include <stddef.h>

#include "canvas.h"
#include "battery_service.h"

#define TODO_SCREEN_ITEM_COUNT 3
#define TODO_SCREEN_TEXT_MAX_LENGTH 24

typedef struct {
    size_t selected_index;
    bool completed[TODO_SCREEN_ITEM_COUNT];
    char items[TODO_SCREEN_ITEM_COUNT][TODO_SCREEN_TEXT_MAX_LENGTH + 1];
} todo_screen_state_t;

void todo_screen_init(todo_screen_state_t *state);
void todo_screen_move(todo_screen_state_t *state, int direction);
void todo_screen_toggle_selected(todo_screen_state_t *state);
void todo_screen_set_item(todo_screen_state_t *state, size_t index, const char *text);
void todo_screen_render(canvas_t *canvas, const todo_screen_state_t *state, const battery_status_t *battery);

#endif
