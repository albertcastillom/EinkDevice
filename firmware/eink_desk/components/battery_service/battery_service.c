#include "battery_service.h"

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_log.h"

#define POWER_I2C_PORT 0
#define POWER_I2C_SDA_GPIO 41
#define POWER_I2C_SCL_GPIO 42
#define POWER_I2C_FREQUENCY_HZ 400000
#define AXP2101_ADDRESS 0x34
#define AXP2101_STATUS1 0x00
#define AXP2101_STATUS2 0x01
#define AXP2101_BATTERY_PERCENT 0xA4

static const char *TAG = "BATTERY";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t axp2101;

static esp_err_t read_register(uint8_t address, uint8_t *value)
{
    return i2c_master_transmit_receive(axp2101, &address, 1, value, 1, 100);
}

esp_err_t battery_service_init(void)
{
    if (axp2101 != NULL) return ESP_OK;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = POWER_I2C_PORT,
        .sda_io_num = POWER_I2C_SDA_GPIO,
        .scl_io_num = POWER_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t result = i2c_new_master_bus(&bus_config, &bus);
    if (result != ESP_OK) return result;

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDRESS,
        .scl_speed_hz = POWER_I2C_FREQUENCY_HZ,
    };
    result = i2c_master_bus_add_device(bus, &device_config, &axp2101);
    if (result == ESP_OK) ESP_LOGI(TAG, "AXP2101 power monitor ready");
    return result;
}

esp_err_t battery_service_read(battery_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    if (axp2101 == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t status1;
    uint8_t status2;
    uint8_t percentage;
    esp_err_t result = read_register(AXP2101_STATUS1, &status1);
    if (result != ESP_OK) return result;
    if ((result = read_register(AXP2101_STATUS2, &status2)) != ESP_OK) return result;

    status->battery_present = (status1 & (1U << 3)) != 0;
    status->usb_connected = (status1 & (1U << 5)) != 0 && (status2 & (1U << 3)) == 0;
    status->charging = (status2 >> 5) == 1;
    status->percentage = -1;
    if (status->battery_present) {
        if ((result = read_register(AXP2101_BATTERY_PERCENT, &percentage)) != ESP_OK) return result;
        status->percentage = percentage <= 100 ? percentage : -1;
    }
    return ESP_OK;
}
