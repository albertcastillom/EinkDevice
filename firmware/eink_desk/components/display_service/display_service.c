#include "display_service.h"

#include <stdbool.h>
#include <stdint.h>

#include "epaper_port.h"

static uint8_t framebuffer[EPD_SIZE_MONO];
static canvas_t display_canvas;
static bool initialized;

esp_err_t display_service_init(void)
{
    if (initialized) return ESP_OK;
    if (!canvas_init(&display_canvas, framebuffer, sizeof(framebuffer), EPD_WIDTH, EPD_HEIGHT)) {
        return ESP_ERR_INVALID_SIZE;
    }
    epaper_port_init();
    initialized = true;
    return ESP_OK;
}

canvas_t *display_service_canvas(void)
{
    return initialized ? &display_canvas : NULL;
}

esp_err_t display_service_present_full(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    EPD_Init();
    EPD_Display_Base(framebuffer);
    EPD_Sleep();
    return ESP_OK;
}

esp_err_t display_service_present_partial(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;
    EPD_Display_Partial(framebuffer, 0, 0, EPD_WIDTH, EPD_HEIGHT);
    EPD_Sleep();
    return ESP_OK;
}
