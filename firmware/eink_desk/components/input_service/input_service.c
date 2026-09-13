#include "input_service.h"

#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define ROCKER_UP_PIN GPIO_NUM_4
#define ROCKER_SELECT_PIN GPIO_NUM_5
#define ROCKER_DOWN_PIN GPIO_NUM_6

#define INPUT_SCAN_INTERVAL_MS 10
#define INPUT_DEBOUNCE_MS 50
#define INPUT_QUEUE_LENGTH 16

typedef struct {
    gpio_num_t pin;
    input_event_t event;
    bool stable_pressed;
    bool candidate_pressed;
    TickType_t candidate_since;
} button_state_t;

static QueueHandle_t event_queue;
static button_state_t buttons[] = {
    {.pin = ROCKER_UP_PIN, .event = INPUT_EVENT_UP},
    {.pin = ROCKER_SELECT_PIN, .event = INPUT_EVENT_SELECT},
    {.pin = ROCKER_DOWN_PIN, .event = INPUT_EVENT_DOWN},
};

static void input_scan_task(void *argument)
{
    (void)argument;

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        for (size_t index = 0; index < sizeof(buttons) / sizeof(buttons[0]); ++index) {
            button_state_t *button = &buttons[index];
            const bool pressed = gpio_get_level(button->pin) == 0;

            if (pressed != button->candidate_pressed) {
                button->candidate_pressed = pressed;
                button->candidate_since = now;
                continue;
            }

            if (pressed == button->stable_pressed ||
                (now - button->candidate_since) < pdMS_TO_TICKS(INPUT_DEBOUNCE_MS)) {
                continue;
            }

            button->stable_pressed = pressed;
            if (pressed) {
                // Only the stable released -> pressed transition creates an event.
                // The button must be released and debounced before it can fire again.
                xQueueSend(event_queue, &button->event, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(INPUT_SCAN_INTERVAL_MS));
    }
}

esp_err_t input_service_init(void)
{
    if (event_queue != NULL) {
        return ESP_OK;
    }

    event_queue = xQueueCreate(INPUT_QUEUE_LENGTH, sizeof(input_event_t));
    if (event_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << ROCKER_UP_PIN) |
                        (1ULL << ROCKER_SELECT_PIN) |
                        (1ULL << ROCKER_DOWN_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) {
        return result;
    }

    for (size_t index = 0; index < sizeof(buttons) / sizeof(buttons[0]); ++index) {
        const bool pressed = gpio_get_level(buttons[index].pin) == 0;
        buttons[index].stable_pressed = pressed;
        buttons[index].candidate_pressed = pressed;
        buttons[index].candidate_since = xTaskGetTickCount();
    }

    if (xTaskCreate(input_scan_task, "rocker_input", 2048, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(event_queue);
        event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool input_service_wait(input_event_t *event, uint32_t timeout_ms)
{
    if (event == NULL || event_queue == NULL) {
        return false;
    }
    return xQueueReceive(event_queue, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

const char *input_service_event_name(input_event_t event)
{
    switch (event) {
        case INPUT_EVENT_UP: return "UP";
        case INPUT_EVENT_SELECT: return "SELECT";
        case INPUT_EVENT_DOWN: return "DOWN";
        default: return "UNKNOWN";
    }
}
