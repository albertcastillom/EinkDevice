#ifndef NETWORK_SERVICE_H
#define NETWORK_SERVICE_H

#include "esp_err.h"
#include "todo_screen.h"

typedef enum {
    NETWORK_STATUS_SETUP,
    NETWORK_STATUS_CONNECTING,
    NETWORK_STATUS_CONNECTED,
    NETWORK_STATUS_CONNECTION_FAILED,
} network_status_t;

typedef esp_err_t (*network_todo_read_fn)(todo_screen_state_t *state, void *context);
typedef esp_err_t (*network_todo_update_fn)(const char items[TODO_SCREEN_ITEM_COUNT][TODO_SCREEN_TEXT_MAX_LENGTH + 1], void *context);

typedef struct {
    network_todo_read_fn read;
    network_todo_update_fn update;
    void *context;
} network_todo_callbacks_t;

esp_err_t network_service_init(const network_todo_callbacks_t *todo_callbacks);
esp_err_t network_service_stop(void);
network_status_t network_service_status(void);
const char *network_service_status_text(void);
const char *network_service_ip_address(void);

#endif
