#include "diagnostics_screen.h"

static void draw_check(canvas_t *canvas, int x, int y)
{
    canvas_draw_rect(canvas, x, y, 30, 30, 3);
    for (int step = 0; step < 10; ++step) {
        canvas_fill_rect(canvas, x + 7 + step, y + 15 + step, 3, 3, true);
        canvas_fill_rect(canvas, x + 16 + step, y + 24 - step, 3, 3, true);
    }
}

void diagnostics_screen_render(canvas_t *canvas)
{
    canvas_clear(canvas, false);
    canvas_draw_rect(canvas, 12, 12, canvas->width - 24, canvas->height - 24, 4);
    canvas_draw_text(canvas, 58, 72, "E-INK DESK", 6);
    canvas_fill_rect(canvas, 30, 142, canvas->width - 60, 3, true);
    draw_check(canvas, 48, 205);
    canvas_draw_text(canvas, 100, 210, "DISPLAY DRIVER: OK", 3);
    draw_check(canvas, 48, 315);
    canvas_draw_text(canvas, 100, 320, "SERIAL OUTPUT: OK", 3);
    draw_check(canvas, 48, 425);
    canvas_draw_text(canvas, 100, 430, "UI LAYERS: READY", 3);
    canvas_fill_rect(canvas, 30, canvas->height - 100, canvas->width - 60, 3, true);
    canvas_draw_text(canvas, 86, canvas->height - 65, "NEXT: ROCKER INPUT", 3);
}
