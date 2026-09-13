#ifndef CANVAS_H
#define CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    size_t buffer_size;
} canvas_t;

bool canvas_init(canvas_t *canvas, uint8_t *pixels, size_t buffer_size, uint16_t width, uint16_t height);
void canvas_clear(canvas_t *canvas, bool black);
void canvas_set_pixel(canvas_t *canvas, int x, int y, bool black);
void canvas_fill_rect(canvas_t *canvas, int x, int y, int width, int height, bool black);
void canvas_draw_rect(canvas_t *canvas, int x, int y, int width, int height, int thickness);
void canvas_draw_char(canvas_t *canvas, int x, int y, char character, int scale);
void canvas_draw_text(canvas_t *canvas, int x, int y, const char *text, int scale);

#endif
