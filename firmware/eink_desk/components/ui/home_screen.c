#include "home_screen.h"

#include <stdio.h>

static const char *HOME_LABELS[HOME_SCREEN_ITEM_COUNT] = {
    "TODO LIST",
    "PHOTO ALBUM",
    "POMODORO",
};

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

static void draw_todo_icon(canvas_t *canvas, int x, int y)
{
    canvas_draw_rect(canvas, x, y, 54, 54, 3);
    for (int row = 0; row < 3; ++row) {
        const int row_y = y + 10 + row * 15;
        canvas_draw_rect(canvas, x + 8, row_y, 9, 9, 2);
        canvas_fill_rect(canvas, x + 24, row_y + 3, 21, 3, true);
    }
}

static void draw_photo_icon(canvas_t *canvas, int x, int y)
{
    canvas_draw_rect(canvas, x, y + 4, 58, 46, 3);
    canvas_fill_rect(canvas, x + 39, y + 13, 8, 8, true);
    for (int step = 0; step < 14; ++step) {
        canvas_fill_rect(canvas, x + 6 + step, y + 42 - step, 3, 3, true);
        canvas_fill_rect(canvas, x + 20 + step, y + 28 + step, 3, 3, true);
        canvas_fill_rect(canvas, x + 34 + step, y + 42 - step, 3, 3, true);
    }
}

static void draw_pomodoro_icon(canvas_t *canvas, int x, int y)
{
    canvas_draw_rect(canvas, x + 8, y + 8, 44, 44, 3);
    canvas_fill_rect(canvas, x + 23, y, 14, 5, true);
    canvas_fill_rect(canvas, x + 28, y + 14, 3, 17, true);
    canvas_fill_rect(canvas, x + 28, y + 28, 13, 3, true);
}

void home_screen_init(home_screen_state_t *state)
{
    if (state != NULL) state->selected_index = 0;
}

void home_screen_move(home_screen_state_t *state, int direction)
{
    if (state == NULL) return;
    if (direction < 0) {
        state->selected_index = state->selected_index == 0
            ? HOME_SCREEN_ITEM_COUNT - 1
            : state->selected_index - 1;
    } else if (direction > 0) {
        state->selected_index = (state->selected_index + 1) % HOME_SCREEN_ITEM_COUNT;
    }
}

home_destination_t home_screen_selected_destination(const home_screen_state_t *state)
{
    if (state == NULL || state->selected_index >= HOME_SCREEN_ITEM_COUNT) return HOME_DESTINATION_TODO;
    return (home_destination_t)state->selected_index;
}

void home_screen_render(canvas_t *canvas, const home_screen_state_t *state, const battery_status_t *battery)
{
    canvas_clear(canvas, false);
    canvas_draw_rect(canvas, 12, 12, canvas->width - 24, canvas->height - 24, 4);
    draw_battery(canvas, battery);
    canvas_draw_text(canvas, 46, 78, "EINK DESK", 6);
    canvas_fill_rect(canvas, 30, 142, canvas->width - 60, 3, true);

    for (size_t index = 0; index < HOME_SCREEN_ITEM_COUNT; ++index) {
        const int y = 185 + (int)index * 150;
        if (state != NULL && index == state->selected_index) {
            canvas_draw_rect(canvas, 32, y - 12, canvas->width - 64, 80, 3);
            canvas_draw_text(canvas, 50, y + 13, ">", 4);
        }

        const int icon_x = 90;
        if (index == HOME_DESTINATION_TODO) draw_todo_icon(canvas, icon_x, y);
        else if (index == HOME_DESTINATION_PHOTOS) draw_photo_icon(canvas, icon_x, y);
        else draw_pomodoro_icon(canvas, icon_x, y);
        canvas_draw_text(canvas, 175, y + 13, HOME_LABELS[index], 4);
    }

    canvas_fill_rect(canvas, 30, canvas->height - 80, canvas->width - 60, 3, true);
    canvas_draw_text(canvas, 66, canvas->height - 52, "UP/DOWN: MOVE  CENTER: OPEN", 2);
}
