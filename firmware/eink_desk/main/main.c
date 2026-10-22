#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "battery_service.h"
#include "display_service.h"
#include "home_screen.h"
#include "input_service.h"
#include "network_service.h"
#include "photo_album_screen.h"
#include "photo_rotation.h"
#include "photo_store.h"
#include "placeholder_screen.h"
#include "todo_screen.h"
#include "todo_store.h"

static const char *TAG = "EINK_DESK";

#define INPUT_BATCH_WINDOW_MS 250
#define IDLE_SLEEP_MS (90 * 1000)
#define WEB_WINDOW_MS (5 * 60 * 1000)
#define PARTIAL_REFRESH_LIMIT 10

typedef enum {
    APP_SCREEN_HOME,
    APP_SCREEN_TODO,
    APP_SCREEN_PHOTOS,
    APP_SCREEN_POMODORO,
} app_screen_t;

static home_screen_state_t home_state;
static photo_album_screen_state_t photo_album_state;
static todo_screen_state_t todo_state;
static app_screen_t current_screen = APP_SCREEN_HOME;
static SemaphoreHandle_t todo_mutex;
static atomic_bool web_update_pending;
static atomic_bool web_activity_pending;

static void web_note_activity(void *context)
{
    (void)context;
    atomic_store(&web_activity_pending, true);
}

static esp_err_t web_read_todos(todo_screen_state_t *state, void *context)
{
    (void)context;
    if (xSemaphoreTake(todo_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
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
    void *context)
{
    (void)context;
    if (xSemaphoreTake(todo_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    for (size_t index = 0; index < TODO_SCREEN_ITEM_COUNT; ++index)
    {
        char cleaned[TODO_SCREEN_TEXT_MAX_LENGTH + 1];
        size_t destination = 0;
        for (size_t source = 0; items[index][source] != '\0' && destination < TODO_SCREEN_TEXT_MAX_LENGTH; ++source)
        {
            cleaned[destination++] = allowed_todo_character(items[index][source]) ? items[index][source] : ' ';
        }
        cleaned[destination] = '\0';
        todo_screen_set_item(&todo_state, index, cleaned);
    }
    const esp_err_t result = todo_store_save(&todo_state);
    if (result == ESP_OK)
        atomic_store(&web_update_pending, true);
    xSemaphoreGive(todo_mutex);
    ESP_LOGI(TAG, "Todo labels updated from the web page");
    return result;
}

static bool apply_input_event(input_event_t event, bool *completed_state_changed, bool *web_requested)
{
    ESP_LOGI(TAG, "Rocker event: %s", input_service_event_name(event));

    if (event == INPUT_EVENT_BACK)
    {
        if (current_screen != APP_SCREEN_HOME)
        {
            current_screen = APP_SCREEN_HOME;
            return true;
        }
        return false;
    }

    if (event == INPUT_EVENT_SELECT_LONG)
    {
        ESP_LOGI(TAG, "Long Select press: enabling web access for five minutes");
        *web_requested = true;
        return false;
    }

    if (current_screen == APP_SCREEN_HOME)
    {
        switch (event)
        {
        case INPUT_EVENT_UP:
            home_screen_move(&home_state, -1);
            return false;
        case INPUT_EVENT_DOWN:
            home_screen_move(&home_state, 1);
            return false;
        case INPUT_EVENT_SELECT:
            switch (home_screen_selected_destination(&home_state))
            {
            case HOME_DESTINATION_TODO:
                current_screen = APP_SCREEN_TODO;
                break;
            case HOME_DESTINATION_PHOTOS:
            {
                const esp_err_t result = photo_album_screen_reload(&photo_album_state);
                if (result != ESP_OK)
                    ESP_LOGW(TAG, "Could not load photo album: %s", esp_err_to_name(result));
                current_screen = APP_SCREEN_PHOTOS;
                break;
            }
            case HOME_DESTINATION_POMODORO:
                current_screen = APP_SCREEN_POMODORO;
                break;
            }
            return true;
        default:
            return false;
        }
    }

    if (current_screen == APP_SCREEN_PHOTOS)
    {
        if (event == INPUT_EVENT_UP)
            photo_album_screen_move(&photo_album_state, -1);
        else if (event == INPUT_EVENT_DOWN)
            photo_album_screen_move(&photo_album_state, 1);
        return false;
    }

    if (current_screen != APP_SCREEN_TODO)
        return false;

    switch (event)
    {
    case INPUT_EVENT_UP:
        todo_screen_move(&todo_state, -1);
        return false;
    case INPUT_EVENT_DOWN:
        todo_screen_move(&todo_state, 1);
        return false;
    case INPUT_EVENT_SELECT:
        todo_screen_toggle_selected(&todo_state);
        *completed_state_changed = true;
        return false;
    default:
        return false;
    }
}

static void render_current_screen(const battery_status_t *battery)
{
    canvas_t *canvas = display_service_canvas();
    switch (current_screen)
    {
    case APP_SCREEN_HOME:
        home_screen_render(canvas, &home_state, battery);
        break;
    case APP_SCREEN_TODO:
        todo_screen_render(canvas, &todo_state, battery);
        break;
    case APP_SCREEN_PHOTOS:
        photo_album_screen_render(canvas, &photo_album_state, battery);
        break;
    case APP_SCREEN_POMODORO:
        placeholder_screen_render(canvas, "POMODORO", battery);
        break;
    }
}

static void render_next_sleep_photo(void)
{
    char filename[PHOTO_STORE_FILENAME_MAX + 1];
    esp_err_t result = photo_rotation_select_next(filename);
    if (result == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGI(TAG, "No uploaded photos; retaining the current screen for sleep");
        return;
    }
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not select a sleep photo: %s", esp_err_to_name(result));
        return;
    }

    canvas_t *canvas = display_service_canvas();
    if (canvas == NULL || canvas->pixels == NULL || canvas->buffer_size != PHOTO_STORE_IMAGE_SIZE)
    {
        ESP_LOGE(TAG, "Display framebuffer is unavailable or has an unexpected size");
        return;
    }

    result = photo_store_read(filename, canvas->pixels, canvas->buffer_size);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not read sleep photo '%s': %s", filename, esp_err_to_name(result));
        return;
    }

    result = display_service_present_full();
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not display sleep photo '%s': %s", filename, esp_err_to_name(result));
        return;
    }
    ESP_LOGI(TAG, "Sleep photo displayed: '%s'", filename);
}

static void enter_deep_sleep(bool network_active)
{
    if (input_service_any_pressed())
        return;
    if (network_active)
    {
        const esp_err_t result = network_service_stop();
        if (result != ESP_OK)
            ESP_LOGW(TAG, "Could not stop network cleanly: %s", esp_err_to_name(result));
    }
    render_next_sleep_photo();
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
    unsigned partial_refresh_count = 0;
    TickType_t web_deadline = 0;
    TickType_t last_activity = xTaskGetTickCount();

    ESP_LOGI(TAG, "Initializing display and rocker input");
    ESP_ERROR_CHECK(display_service_init());
    ESP_ERROR_CHECK(input_service_init());
    ESP_ERROR_CHECK(todo_store_init());
    ESP_ERROR_CHECK(photo_store_init());
    ESP_ERROR_CHECK(battery_service_init());
    ESP_LOGI(
        TAG,
        "PSRAM available: %u bytes total, %u bytes free",
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    home_screen_init(&home_state);
    photo_album_screen_init(&photo_album_state);
    todo_screen_init(&todo_state);
    todo_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(todo_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    esp_err_t load_result = todo_store_load(&todo_state);
    if (load_result == ESP_OK)
    {
        ESP_LOGI(TAG, "Restored completed todos from local storage");
    }
    else if (load_result == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGI(TAG, "No saved todo state yet; using defaults");
    }
    else
    {
        ESP_LOGW(TAG, "Could not load todo state: %s", esp_err_to_name(load_result));
    }
    ESP_ERROR_CHECK(battery_service_read(&battery));
    render_current_screen(&battery);
    if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT1)
    {
        ESP_LOGI(TAG, "Woke from a rocker button; retaining the existing e-paper image");
    }
    else
    {
        ESP_ERROR_CHECK(display_service_present_full());
        display_base_ready = true;
    }
    last_activity = xTaskGetTickCount();
    const network_todo_callbacks_t todo_callbacks = {
        .read = web_read_todos,
        .update = web_update_todos,
        .activity = web_note_activity,
    };
    ESP_LOGI(TAG, "Home screen ready; use the rocker to navigate and BOOT to go back");

    while (true)
    {
        input_event_t event;
        const bool received_input = input_service_wait(&event, 1000);
        const bool needs_refresh = atomic_exchange(&web_update_pending, false);
        const bool web_activity = atomic_exchange(&web_activity_pending, false);

        const TickType_t now = xTaskGetTickCount();
        if (network_active && web_activity)
        {
            web_deadline = now + pdMS_TO_TICKS(WEB_WINDOW_MS);
        }
        if (network_active && (int32_t)(now - web_deadline) >= 0)
        {
            enter_deep_sleep(true);
        }

        if (!network_active && now - last_activity >= pdMS_TO_TICKS(IDLE_SLEEP_MS))
        {
            enter_deep_sleep(false);
        }

        if (!received_input && !needs_refresh)
            continue;

        bool completed_state_changed = false;
        bool input_received = false;
        bool web_requested = false;
        if (received_input)
        {
            last_activity = now;
            xSemaphoreTake(todo_mutex, portMAX_DELAY);
            do
            {
                input_received = true;
                if (event == INPUT_EVENT_RESTART)
                {
                    xSemaphoreGive(todo_mutex);
                    ESP_LOGI(TAG, "Long BOOT press released: restarting");
                    esp_restart();
                }
                apply_input_event(event, &completed_state_changed, &web_requested);
            } while (input_service_wait(&event, INPUT_BATCH_WINDOW_MS));
            xSemaphoreGive(todo_mutex);
        }

        if (web_requested)
        {
            if (!network_active)
            {
                ESP_ERROR_CHECK(network_service_init(&todo_callbacks));
                network_active = true;
            }
            web_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WEB_WINDOW_MS);
        }

        if (completed_state_changed)
        {
            xSemaphoreTake(todo_mutex, portMAX_DELAY);
            const esp_err_t save_result = todo_store_save(&todo_state);
            xSemaphoreGive(todo_mutex);
            if (save_result != ESP_OK)
            {
                ESP_LOGE(TAG, "Could not save todo state: %s", esp_err_to_name(save_result));
            }
        }

        if (!input_received && !(needs_refresh && current_screen == APP_SCREEN_TODO))
            continue;
        xSemaphoreTake(todo_mutex, portMAX_DELAY);
        render_current_screen(&battery);
        xSemaphoreGive(todo_mutex);
        if (display_base_ready && partial_refresh_count < PARTIAL_REFRESH_LIMIT)
        {
            ESP_ERROR_CHECK(display_service_present_partial());
            ++partial_refresh_count;
        }
        else
        {
            // Deep sleep resets the ESP32-side driver state and the panel RAM is
            // not guaranteed to retain a valid partial-refresh base. A periodic
            // full refresh also clears ghosting accumulated by partial updates.
            ESP_ERROR_CHECK(display_service_present_full());
            display_base_ready = true;
            partial_refresh_count = 0;
        }
        ESP_LOGI(TAG, "Rendered app screen: %u", (unsigned)current_screen);
    }
}
