#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include <stdatomic.h>
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
#define IDLE_SLEEP_MS 30000
#define WEB_WINDOW_MS (5 * 60 * 1000)

static todo_screen_state_t todo_state;
static SemaphoreHandle_t todo_mutex;
static atomic_bool web_update_pending;

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
    if (result == ESP_OK) atomic_store(&web_update_pending, true);
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
        case INPUT_EVENT_SELECT_LONG:
            return false;
    }
    return false;
}

static void enter_deep_sleep(bool network_active)
{
    if (input_service_any_pressed()) return;
    if (network_active) {
        const esp_err_t result = network_service_stop();
        if (result != ESP_OK) ESP_LOGW(TAG, "Could not stop network cleanly: %s", esp_err_to_name(result));
    }
    ESP_ERROR_CHECK(input_service_enable_deep_sleep_wakeup());
    ESP_LOGI(TAG, "Entering deep sleep; press any rocker button to wake");
    esp_deep_sleep_start();
}

void app_main(void)
{
    const esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    battery_status_t battery = {.percentage = -1};
    bool display_base_ready = false;
    bool network_active = false;
    TickType_t web_deadline = 0;
    TickType_t last_activity = xTaskGetTickCount();

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
    todo_screen_render(display_service_canvas(), &todo_state, &battery);
    if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) {
        ESP_LOGI(TAG, "Woke from a rocker button; retaining the existing e-paper image");
    } else {
        ESP_ERROR_CHECK(display_service_present_full());
        display_base_ready = true;
    }
    last_activity = xTaskGetTickCount();
    const network_todo_callbacks_t todo_callbacks = {
        .read = web_read_todos,
        .update = web_update_todos,
    };
    ESP_LOGI(TAG, "Todo screen ready; hold Select for two seconds to enable the web page");

    while (true) {
        input_event_t event;
        const bool received_input = input_service_wait(&event, 1000);
        const bool needs_refresh = atomic_exchange(&web_update_pending, false);

        const TickType_t now = xTaskGetTickCount();
        if (network_active && (int32_t)(now - web_deadline) >= 0) {
            enter_deep_sleep(true);
        }

        if (!network_active && now - last_activity >= pdMS_TO_TICKS(IDLE_SLEEP_MS)) {
            enter_deep_sleep(false);
        }

        if (!received_input && !needs_refresh) continue;

        bool completed_state_changed = false;
        bool todo_input_received = false;
        bool web_requested = false;
        if (received_input) {
            last_activity = now;
            xSemaphoreTake(todo_mutex, portMAX_DELAY);
            do {
                if (event == INPUT_EVENT_SELECT_LONG) {
                    ESP_LOGI(TAG, "Long Select press: enabling web access for five minutes");
                    web_requested = true;
                } else {
                    todo_input_received = true;
                    completed_state_changed |= apply_input_event(&todo_state, event);
                }
            } while (input_service_wait(&event, INPUT_BATCH_WINDOW_MS));
            xSemaphoreGive(todo_mutex);
        }

        if (web_requested) {
            if (!network_active) {
                ESP_ERROR_CHECK(network_service_init(&todo_callbacks));
                network_active = true;
            }
            web_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WEB_WINDOW_MS);
        }

        if (completed_state_changed) {
            xSemaphoreTake(todo_mutex, portMAX_DELAY);
            const esp_err_t save_result = todo_store_save(&todo_state);
            xSemaphoreGive(todo_mutex);
            if (save_result != ESP_OK) {
                ESP_LOGE(TAG, "Could not save todo state: %s", esp_err_to_name(save_result));
            }
        }

        todo_screen_state_t render_state;
        if (!todo_input_received && !needs_refresh) continue;
        xSemaphoreTake(todo_mutex, portMAX_DELAY);
        render_state = todo_state;
        xSemaphoreGive(todo_mutex);
        todo_screen_render(display_service_canvas(), &render_state, &battery);
        if (display_base_ready) {
            ESP_ERROR_CHECK(display_service_present_partial());
        } else {
            // Deep sleep resets the ESP32-side driver state and the panel RAM is
            // not guaranteed to retain a valid partial-refresh base. Synchronize
            // it on the first real UI change, not merely because the CPU woke.
            ESP_ERROR_CHECK(display_service_present_full());
            display_base_ready = true;
        }
        ESP_LOGI(TAG, "Selected todo item: %u", (unsigned)(render_state.selected_index + 1));
    }
}
