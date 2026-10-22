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
    const int x = canvas->width - 88;
    const int y = 32;
    canvas_draw_rect(canvas, x, y, 58, 24, 3);
    canvas_fill_rect(canvas, x + 58, y + 7, 5, 10, true);

    if (battery == NULL || !battery->battery_present || battery->percentage < 0) {
        canvas_draw_text(canvas, x - 40, y + 5, "--%", 2);
        return;
    }

    int percentage = battery->percentage;
    if (percentage > 100) percentage = 100;
    const int fill_width = percentage * 48 / 100;
    if (fill_width > 0) canvas_fill_rect(canvas, x + 5, y + 5, fill_width, 14, true);

    char label[8];
    snprintf(label, sizeof(label), "%d%%", percentage);
    canvas_draw_text(canvas, x - 48, y + 5, label, 2);
    if (battery->charging) canvas_draw_text(canvas, x + 68, y + 5, "+", 2);
}

static void draw_wrapped_item(canvas_t *canvas, int x, int y, const char *text)
{
    char first_line[19] = {0};
    char second_line[19] = {0};
    const size_t length = strlen(text);
    size_t split = length > 18 ? 18 : length;

    if (length > 18) {
        while (split > 0 && text[split] != ' ') --split;
        if (split == 0) split = 18;
    }

    memcpy(first_line, text, split);
    first_line[split] = '\0';
    while (split < length && text[split] == ' ') ++split;
    strlcpy(second_line, text + split, sizeof(second_line));

    canvas_draw_text(canvas, x, y, first_line, 3);
    if (second_line[0] != '\0') canvas_draw_text(canvas, x, y + 32, second_line, 3);
}

void todo_screen_render(canvas_t *canvas, const todo_screen_state_t *state, const battery_status_t *battery)
{
    canvas_clear(canvas, false);
    canvas_draw_rect(canvas, 12, 12, canvas->width - 24, canvas->height - 24, 4);
    draw_battery(canvas, battery);
    canvas_draw_text(canvas, 46, 78, "MY TODO LIST", 5);
    canvas_fill_rect(canvas, 30, 142, canvas->width - 60, 3, true);

    for (size_t index = 0; index < TODO_SCREEN_ITEM_COUNT; ++index) {
        const int y = 175 + (int)index * 88;
        if (index == state->selected_index) {
            canvas_draw_text(canvas, 42, y + 14, ">", 4);
            canvas_draw_rect(canvas, 28, y - 8, canvas->width - 56, 70, 2);
        }
        draw_checkbox(canvas, 78, y + 10, state->completed[index]);
        const int text_y = strlen(state->items[index]) > 18 ? y + 2 : y + 16;
        draw_wrapped_item(canvas, 132, text_y, state->items[index]);
    }

    canvas_fill_rect(canvas, 30, canvas->height - 100, canvas->width - 60, 3, true);
    canvas_draw_text(canvas, 72, canvas->height - 72, "UP/DOWN: MOVE  CENTER: CHECK", 2);
    canvas_draw_text(canvas, 54, canvas->height - 42, "BOOT: BACK  HOLD CENTER: WEB", 2);
}
