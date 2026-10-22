#include "photo_album_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char (*filenames)[PHOTO_STORE_FILENAME_MAX + 1];
    size_t capacity;
    size_t count;
} filename_loader_t;

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

static esp_err_t load_filename(const char *filename, void *context)
{
    filename_loader_t *loader = context;
    if (loader->count >= loader->capacity) return ESP_ERR_NO_MEM;
    strlcpy(loader->filenames[loader->count], filename, sizeof(loader->filenames[loader->count]));
    ++loader->count;
    return ESP_OK;
}

static int compare_filenames(const void *left, const void *right)
{
    return strcasecmp(left, right);
}

static void keep_selection_visible(photo_album_screen_state_t *state)
{
    if (state->item_count == 0) {
        state->selected_index = 0;
        state->first_visible_index = 0;
        return;
    }
    if (state->selected_index >= state->item_count) state->selected_index = state->item_count - 1;
    if (state->selected_index < state->first_visible_index) {
        state->first_visible_index = state->selected_index;
    } else if (state->selected_index >= state->first_visible_index + PHOTO_ALBUM_VISIBLE_ITEMS) {
        state->first_visible_index = state->selected_index - PHOTO_ALBUM_VISIBLE_ITEMS + 1;
    }
}

void photo_album_screen_init(photo_album_screen_state_t *state)
{
    if (state == NULL) return;
    *state = (photo_album_screen_state_t){
        .load_result = ESP_OK,
    };
}

esp_err_t photo_album_screen_reload(photo_album_screen_state_t *state)
{
    if (state == NULL) return ESP_ERR_INVALID_ARG;

    photo_store_info_t info = {0};
    esp_err_t result = photo_store_get_info(&info);
    if (result != ESP_OK) {
        state->load_result = result;
        return result;
    }

    // Leave one slot for an upload that commits after the count was read but
    // before the directory listing obtains the store lock.
    const size_t capacity = info.image_count + 1;
    char (*loaded)[PHOTO_STORE_FILENAME_MAX + 1] = calloc(capacity, sizeof(*loaded));
    if (loaded == NULL) {
        state->load_result = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    filename_loader_t loader = {
        .filenames = loaded,
        .capacity = capacity,
    };
    result = photo_store_list(load_filename, &loader);
    if (result != ESP_OK) {
        free(loaded);
        state->load_result = result;
        return result;
    }

    if (loader.count > 0) {
        qsort(loaded, loader.count, sizeof(*loaded), compare_filenames);
    } else {
        free(loaded);
        loaded = NULL;
    }
    free(state->filenames);
    state->filenames = loaded;
    state->item_count = loader.count;
    state->load_result = ESP_OK;
    keep_selection_visible(state);
    return ESP_OK;
}

void photo_album_screen_move(photo_album_screen_state_t *state, int direction)
{
    if (state == NULL || state->item_count == 0) return;
    if (direction < 0) {
        state->selected_index = state->selected_index == 0
            ? state->item_count - 1
            : state->selected_index - 1;
    } else if (direction > 0) {
        state->selected_index = (state->selected_index + 1) % state->item_count;
    }
    keep_selection_visible(state);
}

void photo_album_screen_render(
    canvas_t *canvas,
    const photo_album_screen_state_t *state,
    const battery_status_t *battery)
{
    canvas_clear(canvas, false);
    canvas_draw_rect(canvas, 12, 12, canvas->width - 24, canvas->height - 24, 4);
    draw_battery(canvas, battery);
    canvas_draw_text(canvas, 42, 78, "PHOTO ALBUM", 5);
    canvas_fill_rect(canvas, 30, 142, canvas->width - 60, 3, true);

    if (state == NULL || state->load_result != ESP_OK) {
        canvas_draw_text(canvas, 70, 310, "ALBUM UNAVAILABLE", 4);
    } else if (state->item_count == 0) {
        canvas_draw_text(canvas, 68, 310, "NO IMAGES UPLOADED", 3);
    } else {
        char position[24];
        snprintf(position, sizeof(position), "%u OF %u",
                 (unsigned)(state->selected_index + 1), (unsigned)state->item_count);
        canvas_draw_text(canvas, 42, 158, position, 2);

        const size_t end = state->first_visible_index + PHOTO_ALBUM_VISIBLE_ITEMS < state->item_count
            ? state->first_visible_index + PHOTO_ALBUM_VISIBLE_ITEMS
            : state->item_count;
        for (size_t index = state->first_visible_index; index < end; ++index) {
            const int row = (int)(index - state->first_visible_index);
            const int y = 202 + row * 65;
            if (index == state->selected_index) {
                canvas_draw_rect(canvas, 30, y - 12, canvas->width - 60, 50, 2);
                canvas_draw_text(canvas, 43, y, ">", 3);
            }
            canvas_draw_text(canvas, 76, y + 2, state->filenames[index], 2);
        }
    }

    canvas_fill_rect(canvas, 30, canvas->height - 100, canvas->width - 60, 3, true);
    canvas_draw_text(canvas, 96, canvas->height - 72, "UP/DOWN: MOVE  BOOT: BACK", 2);
    canvas_draw_text(canvas, 108, canvas->height - 42, "HOLD CENTER: WEB", 2);
}
