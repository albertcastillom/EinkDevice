#ifndef PHOTO_ALBUM_SCREEN_H
#define PHOTO_ALBUM_SCREEN_H

#include <stddef.h>

#include "battery_service.h"
#include "canvas.h"
#include "esp_err.h"
#include "photo_store.h"

#define PHOTO_ALBUM_VISIBLE_ITEMS 7U

typedef struct {
    size_t selected_index;
    size_t first_visible_index;
    size_t item_count;
    char (*filenames)[PHOTO_STORE_FILENAME_MAX + 1];
    esp_err_t load_result;
} photo_album_screen_state_t;

void photo_album_screen_init(photo_album_screen_state_t *state);
esp_err_t photo_album_screen_reload(photo_album_screen_state_t *state);
void photo_album_screen_move(photo_album_screen_state_t *state, int direction);
void photo_album_screen_render(
    canvas_t *canvas,
    const photo_album_screen_state_t *state,
    const battery_status_t *battery);

#endif
