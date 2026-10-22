#include "input_service.h"

#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define ROCKER_UP_PIN GPIO_NUM_4
#define ROCKER_SELECT_PIN GPIO_NUM_5
#define ROCKER_DOWN_PIN GPIO_NUM_6
#define BOOT_BACK_PIN GPIO_NUM_0

#define INPUT_DEBOUNCE_MS 15
#define INPUT_LONG_PRESS_MS 2000
#define INPUT_RESTART_PRESS_MS 2000
#define INPUT_QUEUE_LENGTH 16
#define EDGE_QUEUE_LENGTH 16

typedef struct {
    gpio_num_t pin;
    input_event_t event;
    bool stable_pressed;
    bool release_pending;
    TickType_t pressed_since;
} button_state_t;

static QueueHandle_t event_queue;
static QueueHandle_t edge_queue;
static button_state_t buttons[] = {
    {.pin = ROCKER_UP_PIN, .event = INPUT_EVENT_UP},
    {.pin = ROCKER_SELECT_PIN, .event = INPUT_EVENT_SELECT},
    {.pin = ROCKER_DOWN_PIN, .event = INPUT_EVENT_DOWN},
    {.pin = BOOT_BACK_PIN, .event = INPUT_EVENT_BACK},
};

static button_state_t *button_for_pin(gpio_num_t pin)
{
    for (size_t index = 0; index < sizeof(buttons) / sizeof(buttons[0]); ++index) {
        if (buttons[index].pin == pin) return &buttons[index];
    }
    return NULL;
}

static void IRAM_ATTR input_gpio_isr(void *argument)
{
    const gpio_num_t pin = (gpio_num_t)(uintptr_t)argument;
    BaseType_t task_woken = pdFALSE;
    xQueueSendFromISR(edge_queue, &pin, &task_woken);
    if (task_woken == pdTRUE) portYIELD_FROM_ISR();
}

static void input_event_task(void *argument)
{
    (void)argument;
    button_state_t *select_button = button_for_pin(ROCKER_SELECT_PIN);

    while (true) {
        TickType_t wait_ticks = portMAX_DELAY;
        if (select_button->release_pending) {
            const TickType_t elapsed = xTaskGetTickCount() - select_button->pressed_since;
            const TickType_t long_press_ticks = pdMS_TO_TICKS(INPUT_LONG_PRESS_MS);
            wait_ticks = elapsed >= long_press_ticks ? 0 : long_press_ticks - elapsed;
        }

        gpio_num_t pin;
        if (xQueueReceive(edge_queue, &pin, wait_ticks) != pdTRUE) {
            if (select_button->release_pending && gpio_get_level(select_button->pin) == 0) {
                const input_event_t event = INPUT_EVENT_SELECT_LONG;
                select_button->release_pending = false;
                xQueueSend(event_queue, &event, 0);
            }
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(INPUT_DEBOUNCE_MS));

        button_state_t *button = button_for_pin(pin);
        if (button == NULL) continue;
        const bool pressed = gpio_get_level(pin) == 0;
        if (pressed == button->stable_pressed) continue;

        button->stable_pressed = pressed;
        if (pressed && (button->event == INPUT_EVENT_SELECT || button->event == INPUT_EVENT_BACK)) {
            button->release_pending = true;
            button->pressed_since = xTaskGetTickCount();
        } else if (pressed) {
            xQueueSend(event_queue, &button->event, 0);
        } else if (button->event == INPUT_EVENT_SELECT && button->release_pending) {
            const TickType_t duration = xTaskGetTickCount() - button->pressed_since;
            const input_event_t event = duration >= pdMS_TO_TICKS(INPUT_LONG_PRESS_MS)
                                            ? INPUT_EVENT_SELECT_LONG
                                            : INPUT_EVENT_SELECT;
            button->release_pending = false;
            xQueueSend(event_queue, &event, 0);
        } else if (button->event == INPUT_EVENT_BACK && button->release_pending) {
            // GPIO0 is a boot-strapping pin. Restart only after it returns high,
            // otherwise the ESP32-S3 can enter its ROM download mode.
            const TickType_t duration = xTaskGetTickCount() - button->pressed_since;
            const input_event_t event = duration >= pdMS_TO_TICKS(INPUT_RESTART_PRESS_MS)
                                            ? INPUT_EVENT_RESTART
                                            : INPUT_EVENT_BACK;
            button->release_pending = false;
            xQueueSend(event_queue, &event, 0);
        }
    }
}

esp_err_t input_service_init(void)
{
    if (event_queue != NULL) {
        return ESP_OK;
    }

    event_queue = xQueueCreate(INPUT_QUEUE_LENGTH, sizeof(input_event_t));
    edge_queue = xQueueCreate(EDGE_QUEUE_LENGTH, sizeof(gpio_num_t));
    if (event_queue == NULL || edge_queue == NULL) {
        if (event_queue != NULL) vQueueDelete(event_queue);
        if (edge_queue != NULL) vQueueDelete(edge_queue);
        event_queue = NULL;
        edge_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << ROCKER_UP_PIN) |
                        (1ULL << ROCKER_SELECT_PIN) |
                        (1ULL << ROCKER_DOWN_PIN) |
                        (1ULL << BOOT_BACK_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t result = gpio_config(&config);
    if (result != ESP_OK) {
        return result;
    }

    result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    for (size_t index = 0; index < sizeof(buttons) / sizeof(buttons[0]); ++index) {
        buttons[index].stable_pressed = false;
        result = gpio_isr_handler_add(
            buttons[index].pin,
            input_gpio_isr,
            (void *)(uintptr_t)buttons[index].pin
        );
        if (result != ESP_OK) return result;
    }

    if (xTaskCreate(input_event_task, "rocker_input", 2048, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(event_queue);
        vQueueDelete(edge_queue);
        event_queue = NULL;
        edge_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    // A momentary rocker press can end while the ESP32 is still booting from
    // deep sleep. Replay the pin recorded by EXT1 so that wake-up also performs
    // the action that the user requested. For Select, keep using the live edge
    // path when it is still held so a long press can be measured normally.
    uint64_t replayed_wakeup_mask = 0;
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        replayed_wakeup_mask = esp_sleep_get_ext1_wakeup_status();
        for (size_t index = 0; index < sizeof(buttons) / sizeof(buttons[0]); ++index) {
            button_state_t *button = &buttons[index];
            if ((replayed_wakeup_mask & (1ULL << button->pin)) == 0) continue;

            if (button->event == INPUT_EVENT_SELECT && gpio_get_level(button->pin) == 0) {
                const gpio_num_t pin = button->pin;
                xQueueSend(edge_queue, &pin, 0);
            } else {
                xQueueSend(event_queue, &button->event, 0);
            }
        }
    }

    // Also handle a button that was already held during a normal reset. Skip
    // EXT1 pins because they were replayed above.
    for (size_t index = 0; index < sizeof(buttons) / sizeof(buttons[0]); ++index) {
        if ((replayed_wakeup_mask & (1ULL << buttons[index].pin)) == 0 &&
            gpio_get_level(buttons[index].pin) == 0) {
            const gpio_num_t pin = buttons[index].pin;
            xQueueSend(edge_queue, &pin, 0);
        }
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

bool input_service_any_pressed(void)
{
    for (size_t index = 0; index < sizeof(buttons) / sizeof(buttons[0]); ++index) {
        if (gpio_get_level(buttons[index].pin) == 0) return true;
    }
    return false;
}

esp_err_t input_service_enable_deep_sleep_wakeup(void)
{
    const uint64_t wakeup_mask = (1ULL << ROCKER_UP_PIN) |
                                 (1ULL << ROCKER_SELECT_PIN) |
                                 (1ULL << ROCKER_DOWN_PIN) |
                                 (1ULL << BOOT_BACK_PIN);
    for (size_t index = 0; index < sizeof(buttons) / sizeof(buttons[0]); ++index) {
        esp_err_t result = rtc_gpio_pullup_en(buttons[index].pin);
        if (result != ESP_OK) return result;
        result = rtc_gpio_pulldown_dis(buttons[index].pin);
        if (result != ESP_OK) return result;
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    return esp_sleep_enable_ext1_wakeup_io(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
}

const char *input_service_event_name(input_event_t event)
{
    switch (event) {
        case INPUT_EVENT_UP: return "UP";
        case INPUT_EVENT_SELECT: return "SELECT";
        case INPUT_EVENT_SELECT_LONG: return "SELECT_LONG";
        case INPUT_EVENT_DOWN: return "DOWN";
        case INPUT_EVENT_BACK: return "BACK";
        case INPUT_EVENT_RESTART: return "RESTART";
        default: return "UNKNOWN";
    }
}
