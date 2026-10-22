#include "placeholder_screen.h"

#include <stdio.h>

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

void placeholder_screen_render(canvas_t *canvas, const char *title, const battery_status_t *battery)
{
    canvas_clear(canvas, false);
    canvas_draw_rect(canvas, 12, 12, canvas->width - 24, canvas->height - 24, 4);
    draw_battery(canvas, battery);
    canvas_draw_text(canvas, 42, 82, title, 5);
    canvas_fill_rect(canvas, 30, 142, canvas->width - 60, 3, true);

    canvas_draw_rect(canvas, 40, 280, canvas->width - 80, 150, 3);
    canvas_draw_text(canvas, 75, 325, "COMING SOON", 5);

    canvas_fill_rect(canvas, 30, canvas->height - 80, canvas->width - 60, 3, true);
    canvas_draw_text(canvas, 150, canvas->height - 52, "BOOT: BACK", 3);
}
