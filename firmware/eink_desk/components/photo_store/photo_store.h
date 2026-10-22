#ifndef PHOTO_STORE_H
#define PHOTO_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define PHOTO_STORE_IMAGE_SIZE 48000U
#define PHOTO_STORE_FILENAME_MAX 30U

typedef struct photo_store_writer photo_store_writer_t;

typedef struct {
    size_t total_bytes;
    size_t used_bytes;
    size_t image_count;
} photo_store_info_t;

typedef esp_err_t (*photo_store_list_fn)(const char *filename, void *context);

/** Mount the internal flash partition labelled "storage". */
esp_err_t photo_store_init(void);

/** Return true when filename is safe to use and display. */
bool photo_store_filename_is_valid(const char *filename);

/** Return filesystem usage and the number of valid native image files. */
esp_err_t photo_store_get_info(photo_store_info_t *info);

/** Call visitor once for each valid image filename. */
esp_err_t photo_store_list(photo_store_list_fn visitor, void *context);

/** Read one complete display-native 800x480 monochrome image. */
esp_err_t photo_store_read(const char *filename, uint8_t *pixels, size_t size);

/** Delete one image by its displayed filename. */
esp_err_t photo_store_delete(const char *filename);

/** Begin a streamed write. Only one writer may be active at a time. */
esp_err_t photo_store_write_begin(const char *filename, photo_store_writer_t **writer);

/** Append bytes to a streamed write without exceeding PHOTO_STORE_IMAGE_SIZE. */
esp_err_t photo_store_write(photo_store_writer_t *writer, const void *data, size_t size);

/** Commit a write only when exactly PHOTO_STORE_IMAGE_SIZE bytes were received. */
esp_err_t photo_store_write_finish(photo_store_writer_t *writer);

/** Cancel a streamed write and discard its temporary file. */
void photo_store_write_abort(photo_store_writer_t *writer);

#endif
