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
    canvas_draw_text(canvas, 42, 40, "E-INK DESK", 7);
    canvas_fill_rect(canvas, 40, 105, canvas->width - 80, 3, true);
    draw_check(canvas, 48, 145);
    canvas_draw_text(canvas, 100, 147, "DISPLAY DRIVER: OK", 4);
    draw_check(canvas, 48, 215);
    canvas_draw_text(canvas, 100, 217, "SERIAL OUTPUT: OK", 4);
    draw_check(canvas, 48, 285);
    canvas_draw_text(canvas, 100, 287, "UI LAYERS: READY", 4);
    canvas_fill_rect(canvas, 40, 365, canvas->width - 80, 3, true);
    canvas_draw_text(canvas, 48, 395, "NEXT: ROCKER INPUT", 3);
}
