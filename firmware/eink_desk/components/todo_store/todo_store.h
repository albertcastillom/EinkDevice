#ifndef TODO_STORE_H
#define TODO_STORE_H

#include "esp_err.h"
#include "todo_screen.h"

esp_err_t todo_store_init(void);
esp_err_t todo_store_load(todo_screen_state_t *state);
esp_err_t todo_store_save(const todo_screen_state_t *state);

#endif
