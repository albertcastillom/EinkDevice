#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "battery_service.h"
#include "display_service.h"
#include "input_service.h"
#include "network_service.h"
#include "todo_screen.h"
#include "todo_store.h"

static const char *TAG = "EINK_DESK";

#define INPUT_BATCH_WINDOW_MS 250
#define BATTERY_POLL_MS 30000

static todo_screen_state_t todo_state;
static SemaphoreHandle_t todo_mutex;
static volatile bool web_update_pending;

static esp_err_t web_read_todos(todo_screen_state_t *state, void *context)
{
    (void)context;
    if (xSemaphoreTake(todo_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *state = todo_state;
    xSemaphoreGive(todo_mutex);
    return ESP_OK;
}

static bool allowed_todo_character(char character)
{
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || strchr(" .,-!?/()&:+", character) != NULL;
}

static esp_err_t web_update_todos(
    const char items[TODO_SCREEN_ITEM_COUNT][TODO_SCREEN_TEXT_MAX_LENGTH + 1],
    void *context
)
{
    (void)context;
    if (xSemaphoreTake(todo_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    for (size_t index = 0; index < TODO_SCREEN_ITEM_COUNT; ++index) {
        char cleaned[TODO_SCREEN_TEXT_MAX_LENGTH + 1];
        size_t destination = 0;
        for (size_t source = 0; items[index][source] != '\0' && destination < TODO_SCREEN_TEXT_MAX_LENGTH; ++source) {
            cleaned[destination++] = allowed_todo_character(items[index][source]) ? items[index][source] : ' ';
        }
        cleaned[destination] = '\0';
        todo_screen_set_item(&todo_state, index, cleaned);
    }
    const esp_err_t result = todo_store_save(&todo_state);
    web_update_pending = result == ESP_OK;
    xSemaphoreGive(todo_mutex);
    ESP_LOGI(TAG, "Todo labels updated from the web page");
    return result;
}

static bool apply_input_event(todo_screen_state_t *state, input_event_t event)
{
    ESP_LOGI(TAG, "Rocker event: %s", input_service_event_name(event));
    switch (event) {
        case INPUT_EVENT_UP:
            todo_screen_move(state, -1);
            return false;
        case INPUT_EVENT_DOWN:
            todo_screen_move(state, 1);
            return false;
        case INPUT_EVENT_SELECT:
            todo_screen_toggle_selected(state);
            return true;
    }
    return false;
}

void app_main(void)
{
    battery_status_t battery = {.percentage = -1};
    battery_status_t previous_battery = {.percentage = -2};
    TickType_t last_battery_poll = 0;

    ESP_LOGI(TAG, "Initializing display and rocker input");
    ESP_ERROR_CHECK(display_service_init());
    ESP_ERROR_CHECK(input_service_init());
    ESP_ERROR_CHECK(todo_store_init());
    ESP_ERROR_CHECK(battery_service_init());
    ESP_LOGI(
        TAG,
        "PSRAM available: %u bytes total, %u bytes free",
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );

    todo_screen_init(&todo_state);
    todo_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(todo_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    esp_err_t load_result = todo_store_load(&todo_state);
    if (load_result == ESP_OK) {
        ESP_LOGI(TAG, "Restored completed todos from local storage");
    } else if (load_result == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved todo state yet; using defaults");
    } else {
        ESP_LOGW(TAG, "Could not load todo state: %s", esp_err_to_name(load_result));
    }
    ESP_ERROR_CHECK(battery_service_read(&battery));
    previous_battery = battery;
    last_battery_poll = xTaskGetTickCount();
    todo_screen_render(display_service_canvas(), &todo_state, &battery);
    ESP_ERROR_CHECK(display_service_present_full());
    const network_todo_callbacks_t todo_callbacks = {
        .read = web_read_todos,
        .update = web_update_todos,
    };
    ESP_ERROR_CHECK(network_service_init(&todo_callbacks));
    ESP_LOGI(TAG, "Todo screen ready: use GPIO 4/5/6 rocker controls");

    while (true) {
        input_event_t event;
        const bool received_input = input_service_wait(&event, 1000);
        bool needs_refresh = web_update_pending;
        if (web_update_pending) web_update_pending = false;

        const TickType_t now = xTaskGetTickCount();
        if (now - last_battery_poll >= pdMS_TO_TICKS(BATTERY_POLL_MS)) {
            battery_status_t latest;
            const esp_err_t result = battery_service_read(&latest);
            if (result == ESP_OK) {
                needs_refresh |= latest.percentage != previous_battery.percentage ||
                                 latest.charging != previous_battery.charging ||
                                 latest.battery_present != previous_battery.battery_present;
                battery = latest;
                previous_battery = latest;
            } else {
                ESP_LOGW(TAG, "Could not read battery: %s", esp_err_to_name(result));
            }
            last_battery_poll = now;
        }

        if (!received_input && !needs_refresh) continue;

        bool completed_state_changed = false;
        if (received_input) {
            xSemaphoreTake(todo_mutex, portMAX_DELAY);
            completed_state_changed = apply_input_event(&todo_state, event);
            while (input_service_wait(&event, INPUT_BATCH_WINDOW_MS)) {
                completed_state_changed |= apply_input_event(&todo_state, event);
            }
        }

        if (completed_state_changed) {
            const esp_err_t save_result = todo_store_save(&todo_state);
            if (save_result != ESP_OK) {
                ESP_LOGE(TAG, "Could not save todo state: %s", esp_err_to_name(save_result));
            }
        }

        todo_screen_state_t render_state;
        if (received_input) {
            render_state = todo_state;
            xSemaphoreGive(todo_mutex);
        } else {
            xSemaphoreTake(todo_mutex, portMAX_DELAY);
            render_state = todo_state;
            xSemaphoreGive(todo_mutex);
        }
        todo_screen_render(display_service_canvas(), &render_state, &battery);
        ESP_ERROR_CHECK(display_service_present_partial());
        ESP_LOGI(TAG, "Selected todo item: %u", (unsigned)(render_state.selected_index + 1));
    }
}
