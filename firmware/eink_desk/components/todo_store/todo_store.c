#include "todo_store.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TODO_NAMESPACE "eink_desk"
#define TODO_STATE_KEY "todo_state"
#define TODO_STATE_VERSION 2

typedef struct {
    uint8_t version;
    uint8_t completed_mask;
    char items[TODO_SCREEN_ITEM_COUNT][TODO_SCREEN_TEXT_MAX_LENGTH + 1];
} persisted_todo_state_t;

typedef struct {
    uint8_t version;
    uint8_t completed_mask;
} persisted_todo_state_v1_t;

static const char *TAG = "TODO_STORE";
static bool initialized;

esp_err_t todo_store_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs reformatting before it can be used");
        result = nvs_flash_erase();
        if (result == ESP_OK) {
            result = nvs_flash_init();
        }
    }

    if (result == ESP_OK) {
        initialized = true;
    }
    return result;
}

esp_err_t todo_store_load(todo_screen_state_t *state)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(TODO_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (result != ESP_OK) {
        return result;
    }

    size_t saved_size = 0;
    result = nvs_get_blob(handle, TODO_STATE_KEY, NULL, &saved_size);
    if (result == ESP_OK && saved_size == sizeof(persisted_todo_state_v1_t)) {
        persisted_todo_state_v1_t old = {0};
        result = nvs_get_blob(handle, TODO_STATE_KEY, &old, &saved_size);
        nvs_close(handle);
        if (result != ESP_OK) return result;
        if (old.version != 1) return ESP_ERR_INVALID_VERSION;
        for (size_t index = 0; index < TODO_SCREEN_ITEM_COUNT; ++index) {
            state->completed[index] = (old.completed_mask & (1U << index)) != 0;
        }
        ESP_LOGI(TAG, "Loaded version 1 state; keeping the default labels");
        return ESP_OK;
    }

    persisted_todo_state_t saved = {0};
    if (result == ESP_OK && saved_size == sizeof(saved)) {
        result = nvs_get_blob(handle, TODO_STATE_KEY, &saved, &saved_size);
    }
    nvs_close(handle);

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (result != ESP_OK) {
        return result;
    }
    if (saved_size != sizeof(saved) || saved.version != TODO_STATE_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    for (size_t index = 0; index < TODO_SCREEN_ITEM_COUNT; ++index) {
        state->completed[index] = (saved.completed_mask & (1U << index)) != 0;
        todo_screen_set_item(state, index, saved.items[index]);
    }
    return ESP_OK;
}

esp_err_t todo_store_save(const todo_screen_state_t *state)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    persisted_todo_state_t saved = {
        .version = TODO_STATE_VERSION,
        .completed_mask = 0,
    };
    for (size_t index = 0; index < TODO_SCREEN_ITEM_COUNT; ++index) {
        if (state->completed[index]) {
            saved.completed_mask |= (uint8_t)(1U << index);
        }
        strlcpy(saved.items[index], state->items[index], sizeof(saved.items[index]));
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(TODO_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }

    result = nvs_set_blob(handle, TODO_STATE_KEY, &saved, sizeof(saved));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Saved completed todo state: 0x%02x", saved.completed_mask);
    }
    return result;
}
