#include "photo_rotation.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"

#define PHOTO_ROTATION_MAGIC 0x50484f54U
#define PHOTO_ROTATION_MAX_IMAGES 256U
#define PHOTO_ROTATION_BITMAP_SIZE ((PHOTO_ROTATION_MAX_IMAGES + 7U) / 8U)
#define FNV_OFFSET_BASIS UINT64_C(14695981039346656037)
#define FNV_PRIME UINT64_C(1099511628211)

typedef struct {
    uint32_t magic;
    uint64_t catalog_hash;
    size_t catalog_count;
    uint8_t shown[PHOTO_ROTATION_BITMAP_SIZE];
    char last_filename[PHOTO_STORE_FILENAME_MAX + 1];
} rotation_state_t;

typedef struct {
    char (*filenames)[PHOTO_STORE_FILENAME_MAX + 1];
    size_t capacity;
    size_t count;
} filename_loader_t;

static const char *TAG = "PHOTO_ROTATION";
RTC_DATA_ATTR static rotation_state_t rotation_state;

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
    const int insensitive = strcasecmp(left, right);
    return insensitive != 0 ? insensitive : strcmp(left, right);
}

static uint64_t hash_catalog(
    const char (*filenames)[PHOTO_STORE_FILENAME_MAX + 1],
    size_t count)
{
    uint64_t hash = FNV_OFFSET_BASIS;
    for (size_t index = 0; index < count; ++index) {
        const unsigned char *character = (const unsigned char *)filenames[index];
        while (*character != '\0') {
            hash ^= *character++;
            hash *= FNV_PRIME;
        }
        hash ^= 0xffU;
        hash *= FNV_PRIME;
    }
    hash ^= count;
    hash *= FNV_PRIME;
    return hash;
}

static bool was_shown(size_t index)
{
    return (rotation_state.shown[index / 8U] & (uint8_t)(1U << (index % 8U))) != 0;
}

static void mark_shown(size_t index)
{
    rotation_state.shown[index / 8U] |= (uint8_t)(1U << (index % 8U));
}

static size_t count_eligible(
    const char (*filenames)[PHOTO_STORE_FILENAME_MAX + 1],
    size_t count)
{
    size_t eligible = 0;
    for (size_t index = 0; index < count; ++index) {
        const bool is_immediate_repeat = count > 1 &&
            strcmp(filenames[index], rotation_state.last_filename) == 0;
        if (!was_shown(index) && !is_immediate_repeat) ++eligible;
    }
    return eligible;
}

static esp_err_t load_catalog(
    char (**filenames)[PHOTO_STORE_FILENAME_MAX + 1],
    size_t *count)
{
    photo_store_info_t info = {0};
    esp_err_t result = photo_store_get_info(&info);
    if (result != ESP_OK) return result;
    if (info.image_count == 0) return ESP_ERR_NOT_FOUND;
    if (info.image_count > PHOTO_ROTATION_MAX_IMAGES) return ESP_ERR_INVALID_SIZE;

    // One extra entry tolerates an upload committing between the count and list
    // operations. The partition cannot hold more than the hard maximum above.
    const size_t capacity = info.image_count < PHOTO_ROTATION_MAX_IMAGES
        ? info.image_count + 1
        : info.image_count;
    char (*loaded)[PHOTO_STORE_FILENAME_MAX + 1] = calloc(capacity, sizeof(*loaded));
    if (loaded == NULL) return ESP_ERR_NO_MEM;

    filename_loader_t loader = {
        .filenames = loaded,
        .capacity = capacity,
    };
    result = photo_store_list(load_filename, &loader);
    if (result != ESP_OK) {
        free(loaded);
        return result;
    }
    if (loader.count == 0) {
        free(loaded);
        return ESP_ERR_NOT_FOUND;
    }
    if (loader.count > PHOTO_ROTATION_MAX_IMAGES) {
        free(loaded);
        return ESP_ERR_INVALID_SIZE;
    }

    qsort(loaded, loader.count, sizeof(*loaded), compare_filenames);
    *filenames = loaded;
    *count = loader.count;
    return ESP_OK;
}

esp_err_t photo_rotation_select_next(char filename[PHOTO_STORE_FILENAME_MAX + 1])
{
    if (filename == NULL) return ESP_ERR_INVALID_ARG;
    filename[0] = '\0';

    char (*filenames)[PHOTO_STORE_FILENAME_MAX + 1] = NULL;
    size_t count = 0;
    esp_err_t result = load_catalog(&filenames, &count);
    if (result != ESP_OK) return result;

    const uint64_t catalog_hash = hash_catalog((const char (*)[PHOTO_STORE_FILENAME_MAX + 1])filenames, count);
    rotation_state.last_filename[PHOTO_STORE_FILENAME_MAX] = '\0';
    if (rotation_state.magic != PHOTO_ROTATION_MAGIC ||
        rotation_state.catalog_hash != catalog_hash ||
        rotation_state.catalog_count != count) {
        const bool state_was_valid = rotation_state.magic == PHOTO_ROTATION_MAGIC;
        char previous[PHOTO_STORE_FILENAME_MAX + 1] = {0};
        if (state_was_valid) strlcpy(previous, rotation_state.last_filename, sizeof(previous));
        rotation_state = (rotation_state_t){
            .magic = PHOTO_ROTATION_MAGIC,
            .catalog_hash = catalog_hash,
            .catalog_count = count,
        };
        strlcpy(rotation_state.last_filename, previous, sizeof(rotation_state.last_filename));
        ESP_LOGI(TAG, "Started a new rotation for %u photos", (unsigned)count);
    }

    size_t eligible = count_eligible(
        (const char (*)[PHOTO_STORE_FILENAME_MAX + 1])filenames,
        count);
    if (eligible == 0) {
        memset(rotation_state.shown, 0, sizeof(rotation_state.shown));
        eligible = count_eligible(
            (const char (*)[PHOTO_STORE_FILENAME_MAX + 1])filenames,
            count);
    }
    if (eligible == 0) {
        // Only possible for a one-photo album whose single entry was somehow
        // retained as shown; allow it because a non-repeat is impossible.
        memset(rotation_state.shown, 0, sizeof(rotation_state.shown));
        eligible = count;
    }

    const size_t choice = (size_t)(esp_random() % eligible);
    size_t seen = 0;
    size_t selected = 0;
    for (size_t index = 0; index < count; ++index) {
        const bool is_immediate_repeat = count > 1 &&
            strcmp(filenames[index], rotation_state.last_filename) == 0;
        if (was_shown(index) || is_immediate_repeat) continue;
        if (seen == choice) {
            selected = index;
            break;
        }
        ++seen;
    }

    mark_shown(selected);
    strlcpy(rotation_state.last_filename, filenames[selected], sizeof(rotation_state.last_filename));
    strlcpy(filename, filenames[selected], PHOTO_STORE_FILENAME_MAX + 1);
    ESP_LOGI(TAG, "Selected '%s' for the next sleep image", filename);
    free(filenames);
    return ESP_OK;
}
