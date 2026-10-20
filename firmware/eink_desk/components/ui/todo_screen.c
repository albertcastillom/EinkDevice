#include "todo_screen.h"

#include <stdio.h>
#include <string.h>

static const char *DEFAULT_TODO_ITEMS[TODO_SCREEN_ITEM_COUNT] = {
    "BUILD INPUT DRIVER",
    "CREATE TODO SCREEN",
    "ADD WI-FI SYNC",
};

static void draw_checkbox(canvas_t *canvas, int x, int y, bool checked)
{
    canvas_draw_rect(canvas, x, y, 34, 34, 3);
    if (!checked) return;

    for (int step = 0; step < 11; ++step) {
        canvas_fill_rect(canvas, x + 7 + step, y + 16 + step, 3, 3, true);
        canvas_fill_rect(canvas, x + 17 + step, y + 26 - step, 3, 3, true);
    }
}

void todo_screen_init(todo_screen_state_t *state)
{
    *state = (todo_screen_state_t) {
        .selected_index = 0,
        .completed = {true, false, false},
    };
    for (size_t index = 0; index < TODO_SCREEN_ITEM_COUNT; ++index) {
        todo_screen_set_item(state, index, DEFAULT_TODO_ITEMS[index]);
    }
}

void todo_screen_move(todo_screen_state_t *state, int direction)
{
    if (direction < 0) {
        state->selected_index = state->selected_index == 0
            ? TODO_SCREEN_ITEM_COUNT - 1
            : state->selected_index - 1;
    } else if (direction > 0) {
        state->selected_index = (state->selected_index + 1) % TODO_SCREEN_ITEM_COUNT;
    }
}

void todo_screen_toggle_selected(todo_screen_state_t *state)
{
    state->completed[state->selected_index] = !state->completed[state->selected_index];
}

void todo_screen_set_item(todo_screen_state_t *state, size_t index, const char *text)
{
    if (state == NULL || text == NULL || index >= TODO_SCREEN_ITEM_COUNT) return;
    strlcpy(state->items[index], text, sizeof(state->items[index]));
}

static void draw_battery(canvas_t *canvas, const battery_status_t *battery)
{
    const int x = 640;
    const int y = 42;
    canvas_draw_rect(canvas, x, y, 72, 30, 3);
    canvas_fill_rect(canvas, x + 72, y + 9, 6, 12, true);

    if (battery == NULL || !battery->battery_present || battery->percentage < 0) {
        canvas_draw_text(canvas, x - 42, y + 5, "--%", 3);
        return;
    }

    int percentage = battery->percentage;
    if (percentage > 100) percentage = 100;
    const int fill_width = percentage * 62 / 100;
    if (fill_width > 0) canvas_fill_rect(canvas, x + 5, y + 5, fill_width, 20, true);

    char label[8];
    snprintf(label, sizeof(label), "%d%%", percentage);
    canvas_draw_text(canvas, x - 70, y + 5, label, 3);
    if (battery->charging) canvas_draw_text(canvas, x + 84, y + 5, "+", 3);
}

void todo_screen_render(canvas_t *canvas, const todo_screen_state_t *state, const battery_status_t *battery)
{
    canvas_clear(canvas, false);
    canvas_draw_rect(canvas, 12, 12, canvas->width - 24, canvas->height - 24, 4);
    canvas_draw_text(canvas, 42, 38, "MY TODO LIST", 7);
    draw_battery(canvas, battery);
    canvas_fill_rect(canvas, 40, 102, canvas->width - 80, 3, true);

    for (size_t index = 0; index < TODO_SCREEN_ITEM_COUNT; ++index) {
        const int y = 135 + (int)index * 82;
        if (index == state->selected_index) {
            canvas_draw_text(canvas, 38, y + 4, ">", 4);
            canvas_draw_rect(canvas, 28, y - 12, canvas->width - 56, 60, 2);
        }
        draw_checkbox(canvas, 78, y, state->completed[index]);
        canvas_draw_text(canvas, 135, y + 3, state->items[index], 4);
    }

    canvas_fill_rect(canvas, 40, 391, canvas->width - 80, 3, true);
    canvas_draw_text(canvas, 48, 416, "UP/DOWN: MOVE  CENTER: CHECK", 3);
    canvas_draw_text(canvas, 48, 446, "HOLD CENTER 2 SEC: WEB FOR 5 MIN", 2);
}
