#include "photo_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define PHOTO_PARTITION_LABEL "storage"
#define PHOTO_MOUNT_PATH "/photos"
#define PHOTO_TEMP_PATH PHOTO_MOUNT_PATH "/.upload"
#define PHOTO_PATH_SIZE (sizeof(PHOTO_MOUNT_PATH) + PHOTO_STORE_FILENAME_MAX + 1U)

struct photo_store_writer {
    FILE *file;
    char target_path[PHOTO_PATH_SIZE];
    size_t bytes_written;
};

static const char *TAG = "PHOTO_STORE";
static SemaphoreHandle_t store_mutex;
static bool initialized;

static esp_err_t require_initialized(void)
{
    return initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t lock_store(void)
{
    if (store_mutex == NULL) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(store_mutex, pdMS_TO_TICKS(1000)) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
}

static void unlock_store(void)
{
    xSemaphoreGive(store_mutex);
}

static void make_path(const char *filename, char path[PHOTO_PATH_SIZE])
{
    snprintf(path, PHOTO_PATH_SIZE, "%s/%s", PHOTO_MOUNT_PATH, filename);
}

static bool file_is_native_image(const char *filename)
{
    if (!photo_store_filename_is_valid(filename)) return false;
    char path[PHOTO_PATH_SIZE];
    make_path(filename, path);
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_size == PHOTO_STORE_IMAGE_SIZE;
}

esp_err_t photo_store_init(void)
{
    if (initialized) return ESP_OK;

    store_mutex = xSemaphoreCreateMutex();
    if (store_mutex == NULL) return ESP_ERR_NO_MEM;

    const esp_vfs_spiffs_conf_t configuration = {
        .base_path = PHOTO_MOUNT_PATH,
        .partition_label = PHOTO_PARTITION_LABEL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    const esp_err_t result = esp_vfs_spiffs_register(&configuration);
    if (result != ESP_OK) {
        vSemaphoreDelete(store_mutex);
        store_mutex = NULL;
        ESP_LOGE(TAG, "Could not mount photo storage: %s", esp_err_to_name(result));
        return result;
    }

    initialized = true;
    photo_store_info_t info;
    if (photo_store_get_info(&info) == ESP_OK) {
        ESP_LOGI(TAG, "Photo storage ready: %u images, %u/%u bytes used",
                 (unsigned)info.image_count, (unsigned)info.used_bytes,
                 (unsigned)info.total_bytes);
    }
    return ESP_OK;
}

bool photo_store_filename_is_valid(const char *filename)
{
    if (filename == NULL) return false;
    const size_t length = strnlen(filename, PHOTO_STORE_FILENAME_MAX + 1U);
    if (length == 0 || length > PHOTO_STORE_FILENAME_MAX || filename[0] == '.') return false;

    for (size_t index = 0; index < length; ++index) {
        const unsigned char character = (unsigned char)filename[index];
        if (!isalnum(character) && character != ' ' && character != '-' &&
            character != '_' && character != '.' && character != '(' && character != ')') {
            return false;
        }
    }
    return true;
}

esp_err_t photo_store_get_info(photo_store_info_t *info)
{
    if (require_initialized() != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (info == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = lock_store();
    if (result != ESP_OK) return result;

    size_t total = 0;
    size_t used = 0;
    result = esp_spiffs_info(PHOTO_PARTITION_LABEL, &total, &used);
    size_t count = 0;
    if (result == ESP_OK) {
        DIR *directory = opendir(PHOTO_MOUNT_PATH);
        if (directory == NULL) {
            result = ESP_FAIL;
        } else {
            const struct dirent *entry;
            while ((entry = readdir(directory)) != NULL) {
                if (file_is_native_image(entry->d_name)) ++count;
            }
            closedir(directory);
        }
    }
    if (result == ESP_OK) {
        *info = (photo_store_info_t){
            .total_bytes = total,
            .used_bytes = used,
            .image_count = count,
        };
    }
    unlock_store();
    return result;
}

esp_err_t photo_store_list(photo_store_list_fn visitor, void *context)
{
    if (require_initialized() != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (visitor == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = lock_store();
    if (result != ESP_OK) return result;

    DIR *directory = opendir(PHOTO_MOUNT_PATH);
    if (directory == NULL) {
        result = ESP_FAIL;
    } else {
        const struct dirent *entry;
        while (result == ESP_OK && (entry = readdir(directory)) != NULL) {
            if (file_is_native_image(entry->d_name)) {
                result = visitor(entry->d_name, context);
            }
        }
        closedir(directory);
    }
    unlock_store();
    return result;
}

esp_err_t photo_store_read(const char *filename, uint8_t *pixels, size_t size)
{
    if (require_initialized() != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (!photo_store_filename_is_valid(filename) || pixels == NULL) return ESP_ERR_INVALID_ARG;
    if (size != PHOTO_STORE_IMAGE_SIZE) return ESP_ERR_INVALID_SIZE;
    esp_err_t result = lock_store();
    if (result != ESP_OK) return result;

    char path[PHOTO_PATH_SIZE];
    make_path(filename, path);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        result = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    } else {
        result = fread(pixels, 1, size, file) == size && fgetc(file) == EOF
            ? ESP_OK
            : ESP_ERR_INVALID_SIZE;
        fclose(file);
    }
    unlock_store();
    return result;
}

esp_err_t photo_store_delete(const char *filename)
{
    if (require_initialized() != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (!photo_store_filename_is_valid(filename)) return ESP_ERR_INVALID_ARG;
    esp_err_t result = lock_store();
    if (result != ESP_OK) return result;

    char path[PHOTO_PATH_SIZE];
    make_path(filename, path);
    if (remove(path) != 0) {
        result = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "Deleted photo '%s'", filename);
    }
    unlock_store();
    return result;
}

esp_err_t photo_store_write_begin(const char *filename, photo_store_writer_t **writer)
{
    if (require_initialized() != ESP_OK) return ESP_ERR_INVALID_STATE;
    if (!photo_store_filename_is_valid(filename) || writer == NULL) return ESP_ERR_INVALID_ARG;
    *writer = NULL;

    esp_err_t result = lock_store();
    if (result != ESP_OK) return result;

    size_t total = 0;
    size_t used = 0;
    result = esp_spiffs_info(PHOTO_PARTITION_LABEL, &total, &used);
    char target_path[PHOTO_PATH_SIZE];
    make_path(filename, target_path);
    // The temporary upload and an existing image coexist until commit, so a
    // replacement also needs enough free space for one complete frame.
    if (result == ESP_OK && (used > total || total - used < PHOTO_STORE_IMAGE_SIZE)) {
        result = ESP_ERR_NO_MEM;
    }

    photo_store_writer_t *new_writer = NULL;
    if (result == ESP_OK) {
        new_writer = calloc(1, sizeof(*new_writer));
        if (new_writer == NULL) result = ESP_ERR_NO_MEM;
    }
    if (result == ESP_OK) {
        remove(PHOTO_TEMP_PATH);
        new_writer->file = fopen(PHOTO_TEMP_PATH, "wb");
        if (new_writer->file == NULL) result = ESP_FAIL;
    }
    if (result != ESP_OK) {
        free(new_writer);
        unlock_store();
        return result;
    }

    strlcpy(new_writer->target_path, target_path, sizeof(new_writer->target_path));
    *writer = new_writer;
    return ESP_OK;
}

esp_err_t photo_store_write(photo_store_writer_t *writer, const void *data, size_t size)
{
    if (writer == NULL || writer->file == NULL || (data == NULL && size != 0)) return ESP_ERR_INVALID_ARG;
    if (size > PHOTO_STORE_IMAGE_SIZE - writer->bytes_written) return ESP_ERR_INVALID_SIZE;
    if (size != 0 && fwrite(data, 1, size, writer->file) != size) return ESP_FAIL;
    writer->bytes_written += size;
    return ESP_OK;
}

esp_err_t photo_store_write_finish(photo_store_writer_t *writer)
{
    if (writer == NULL || writer->file == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = writer->bytes_written == PHOTO_STORE_IMAGE_SIZE
        ? ESP_OK
        : ESP_ERR_INVALID_SIZE;
    if (result == ESP_OK && fflush(writer->file) != 0) result = ESP_FAIL;
    if (fclose(writer->file) != 0 && result == ESP_OK) result = ESP_FAIL;
    writer->file = NULL;

    if (result == ESP_OK) {
        remove(writer->target_path);
        if (rename(PHOTO_TEMP_PATH, writer->target_path) != 0) result = ESP_FAIL;
    }
    if (result != ESP_OK) remove(PHOTO_TEMP_PATH);
    if (result == ESP_OK) ESP_LOGI(TAG, "Saved photo '%s'", writer->target_path);
    free(writer);
    unlock_store();
    return result;
}

void photo_store_write_abort(photo_store_writer_t *writer)
{
    if (writer == NULL) return;
    if (writer->file != NULL) fclose(writer->file);
    remove(PHOTO_TEMP_PATH);
    free(writer);
    unlock_store();
}
