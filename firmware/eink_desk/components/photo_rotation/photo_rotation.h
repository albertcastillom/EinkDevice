#ifndef PHOTO_ROTATION_H
#define PHOTO_ROTATION_H

#include "esp_err.h"
#include "photo_store.h"

/**
 * Select the next filename in a randomized, non-repeating album cycle.
 *
 * The cycle survives deep sleep in RTC memory. A changed album automatically
 * starts a new cycle while still avoiding the last displayed photo when
 * possible.
 */
esp_err_t photo_rotation_select_next(char filename[PHOTO_STORE_FILENAME_MAX + 1]);

#endif
