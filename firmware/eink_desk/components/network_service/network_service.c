#include "network_service.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"

#define SETUP_AP_SSID "EinkDesk-Setup"
#define SETUP_AP_PASSWORD "einkdesk"
#define MAXIMUM_RETRY_COUNT 10
#define FORM_BODY_SIZE 256

static const char *TAG = "NETWORK";
static network_status_t current_status = NETWORK_STATUS_SETUP;
static char station_ip[24] = "not connected";
static bool station_configured;
static unsigned retry_count;
static httpd_handle_t http_server;
static network_todo_callbacks_t todos;
static esp_netif_t *access_point_netif;
static esp_netif_t *station_netif;

static void html_escape(const char *source, char *destination, size_t size)
{
    if (size == 0) return;
    while (*source != '\0' && size > 1) {
        const char *replacement = NULL;
        if (*source == '&') replacement = "&amp;";
        else if (*source == '<') replacement = "&lt;";
        else if (*source == '>') replacement = "&gt;";
        else if (*source == '\'') replacement = "&#39;";
        else if (*source == '"') replacement = "&quot;";
        if (replacement != NULL) {
            const size_t length = strlen(replacement);
            if (length >= size) break;
            memcpy(destination, replacement, length);
            destination += length;
            size -= length;
        } else {
            *destination++ = *source;
            --size;
        }
        ++source;
    }
    *destination = '\0';
}

static void url_decode(char *value)
{
    char *source = value;
    char *destination = value;
    while (*source != '\0') {
        if (*source == '+') {
            *destination++ = ' ';
            ++source;
        } else if (*source == '%' && isxdigit((unsigned char)source[1]) &&
                   isxdigit((unsigned char)source[2])) {
            char encoded[3] = {source[1], source[2], '\0'};
            *destination++ = (char)strtol(encoded, NULL, 16);
            source += 3;
        } else {
            *destination++ = *source++;
        }
    }
    *destination = '\0';
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    static char page[4096];
    todo_screen_state_t todo_state = {0};
    char escaped[TODO_SCREEN_ITEM_COUNT][TODO_SCREEN_TEXT_MAX_LENGTH * 6 + 1] = {0};
    if (todos.read != NULL && todos.read(&todo_state, todos.context) == ESP_OK) {
        for (size_t index = 0; index < TODO_SCREEN_ITEM_COUNT; ++index) {
            html_escape(todo_state.items[index], escaped[index], sizeof(escaped[index]));
        }
    }
    const int length = snprintf(
        page,
        sizeof(page),
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>E-Ink Desk</title><style>body{font-family:system-ui;max-width:620px;margin:40px auto;"
        "padding:0 20px;background:#f4f1e8;color:#171717}main{background:white;padding:28px;border:2px solid #171717;"
        "box-shadow:6px 6px 0 #171717}h1{margin-top:0}label{display:block;margin-top:18px;font-weight:700}"
        "input{box-sizing:border-box;width:100%%;padding:12px;margin-top:6px;border:2px solid #444;font-size:16px}"
        "button{margin-top:22px;padding:12px 18px;background:#171717;color:white;border:0;font-weight:700;font-size:16px}"
        ".status{padding:12px;background:#eee;border-left:5px solid #171717}hr{margin:30px 0;border:0;border-top:2px solid #171717}"
        ".done{color:#555}</style></head><body><main>"
        "<h1>E-Ink Desk</h1><p class='status'>Status: %s<br>Device IP: %s</p>"
        "<h2>Todo list</h2><p>Edit the labels below, then save to update the e-paper screen.</p>"
        "<form method='post' action='/todos'>"
        "<label>Todo 1%s</label><input name='t0' maxlength='%d' value='%s' required>"
        "<label>Todo 2%s</label><input name='t1' maxlength='%d' value='%s' required>"
        "<label>Todo 3%s</label><input name='t2' maxlength='%d' value='%s' required>"
        "<button type='submit'>Update display</button></form><hr><h2>Wi-Fi</h2>"
        "<p>Enter the Wi-Fi network this desk display should use.</p>"
        "<form method='post' action='/wifi'><label>Network name</label><input name='ssid' maxlength='32' required>"
        "<label>Password</label><input name='password' type='password' maxlength='63'>"
        "<button type='submit'>Save and connect</button></form></main></body></html>",
        network_service_status_text(),
        station_ip,
        todo_state.completed[0] ? " (completed)" : "", TODO_SCREEN_TEXT_MAX_LENGTH, escaped[0],
        todo_state.completed[1] ? " (completed)" : "", TODO_SCREEN_TEXT_MAX_LENGTH, escaped[1],
        todo_state.completed[2] ? " (completed)" : "", TODO_SCREEN_TEXT_MAX_LENGTH, escaped[2]
    );
    if (length < 0 || length >= sizeof(page)) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Page generation failed");
    }

    httpd_resp_set_type(request, "text/html");
    return httpd_resp_send(request, page, length);
}

static esp_err_t todos_post_handler(httpd_req_t *request)
{
    if (todos.update == NULL || request->content_len <= 0 || request->content_len >= FORM_BODY_SIZE) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid todo data");
    }
    char body[FORM_BODY_SIZE] = {0};
    size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count <= 0) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read form data");
        received += (size_t)count;
    }

    char items[TODO_SCREEN_ITEM_COUNT][TODO_SCREEN_TEXT_MAX_LENGTH + 1] = {0};
    for (size_t index = 0; index < TODO_SCREEN_ITEM_COUNT; ++index) {
        char key[4];
        snprintf(key, sizeof(key), "t%u", (unsigned)index);
        if (httpd_query_key_value(body, key, items[index], sizeof(items[index])) != ESP_OK) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "All three todos are required");
        }
        url_decode(items[index]);
        if (items[index][0] == '\0') return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Todos cannot be empty");
    }

    const esp_err_t result = todos.update(items, todos.context);
    if (result != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save todos");
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_sendstr(request, "Todos saved");
}

static esp_err_t todos_api_get_handler(httpd_req_t *request)
{
    todo_screen_state_t state = {0};
    if (todos.read == NULL || todos.read(&state, todos.context) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Todo state unavailable");
    }
    char json[512];
    const int length = snprintf(json, sizeof(json),
        "{\"todos\":[{\"text\":\"%s\",\"completed\":%s},{\"text\":\"%s\",\"completed\":%s},"
        "{\"text\":\"%s\",\"completed\":%s}]}",
        state.items[0], state.completed[0] ? "true" : "false",
        state.items[1], state.completed[1] ? "true" : "false",
        state.items[2], state.completed[2] ? "true" : "false");
    if (length < 0 || length >= sizeof(json)) return ESP_FAIL;
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, json, length);
}

static esp_err_t wifi_post_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len >= FORM_BODY_SIZE) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form data");
    }

    char body[FORM_BODY_SIZE] = {0};
    size_t received = 0;
    while (received < request->content_len) {
        const int result = httpd_req_recv(request, body + received, request->content_len - received);
        if (result <= 0) {
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read form data");
        }
        received += (size_t)result;
    }

    char ssid[33] = {0};
    char password[64] = {0};
    if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "A network name is required");
    }
    httpd_query_key_value(body, "password", password, sizeof(password));
    url_decode(ssid);
    url_decode(password);

    wifi_config_t station_config = {0};
    strlcpy((char *)station_config.sta.ssid, ssid, sizeof(station_config.sta.ssid));
    strlcpy((char *)station_config.sta.password, password, sizeof(station_config.sta.password));
    station_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    station_config.sta.pmf_cfg.capable = true;
    station_config.sta.pmf_cfg.required = false;

    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &station_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Wi-Fi configuration: %s", esp_err_to_name(result));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save Wi-Fi settings");
    }

    station_configured = true;
    retry_count = 0;
    current_status = NETWORK_STATUS_CONNECTING;
    strlcpy(station_ip, "connecting", sizeof(station_ip));
    result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not enable Wi-Fi client");
    }
    esp_wifi_disconnect();
    result = esp_wifi_connect();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not begin Wi-Fi connection: %s", esp_err_to_name(result));
    }

    ESP_LOGI(TAG, "Saved Wi-Fi network '%s'; connecting", ssid);
    httpd_resp_set_type(request, "text/html");
    return httpd_resp_sendstr(
        request,
        "<!doctype html><html><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<body style='font-family:system-ui;max-width:560px;margin:40px auto;padding:20px'>"
        "<h1>Settings saved</h1><p>The E-Ink Desk is connecting. You can return to "
        "<a href='/'>the status page</a> in a few seconds.</p></body></html>"
    );
}

static void network_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START && station_configured) {
        current_status = NETWORK_STATUS_CONNECTING;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED && station_configured) {
        station_ip[0] = '\0';
        if (retry_count < MAXIMUM_RETRY_COUNT) {
            ++retry_count;
            current_status = NETWORK_STATUS_CONNECTING;
            esp_wifi_connect();
            ESP_LOGW(TAG, "Wi-Fi disconnected; retry %u/%u", retry_count, MAXIMUM_RETRY_COUNT);
        } else {
            current_status = NETWORK_STATUS_CONNECTION_FAILED;
            strlcpy(station_ip, "connection failed", sizeof(station_ip));
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            ESP_LOGE(TAG, "Wi-Fi connection failed; setup access point remains available");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        snprintf(station_ip, sizeof(station_ip), IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        current_status = NETWORK_STATUS_CONNECTED;
        ESP_LOGI(TAG, "Connected to Wi-Fi; open http://%s", station_ip);
    }
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t result = httpd_start(&http_server, &config);
    if (result != ESP_OK) {
        return result;
    }

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t wifi_uri = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = wifi_post_handler,
    };
    const httpd_uri_t todos_uri = {.uri = "/todos", .method = HTTP_POST, .handler = todos_post_handler};
    const httpd_uri_t todos_api_uri = {.uri = "/api/todos", .method = HTTP_GET, .handler = todos_api_get_handler};
    result = httpd_register_uri_handler(http_server, &root_uri);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(http_server, &wifi_uri);
    }
    if (result == ESP_OK) result = httpd_register_uri_handler(http_server, &todos_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(http_server, &todos_api_uri);
    return result;
}

esp_err_t network_service_init(const network_todo_callbacks_t *todo_callbacks)
{
    if (todo_callbacks == NULL || todo_callbacks->read == NULL || todo_callbacks->update == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    todos = *todo_callbacks;
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }

    access_point_netif = esp_netif_create_default_wifi_ap();
    station_netif = esp_netif_create_default_wifi_sta();
    if (access_point_netif == NULL || station_netif == NULL) return ESP_ERR_NO_MEM;

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    if ((result = esp_wifi_init(&initialization)) != ESP_OK) return result;
    if ((result = esp_wifi_set_storage(WIFI_STORAGE_FLASH)) != ESP_OK) return result;

    wifi_config_t saved_station = {0};
    if ((result = esp_wifi_get_config(WIFI_IF_STA, &saved_station)) != ESP_OK) return result;
    station_configured = saved_station.sta.ssid[0] != '\0';

    wifi_config_t access_point = {
        .ap = {
            .ssid = SETUP_AP_SSID,
            .password = SETUP_AP_PASSWORD,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.required = false},
        },
    };
    // The setup AP exists only during the explicit five-minute web window, so
    // keep it reachable even while the station joins a saved network.
    const wifi_mode_t initial_mode = station_configured ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    if ((result = esp_wifi_set_mode(initial_mode)) != ESP_OK) return result;
    if ((result = esp_wifi_set_config(WIFI_IF_AP, &access_point)) != ESP_OK) return result;

    if ((result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL)) != ESP_OK) {
        return result;
    }
    if ((result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event_handler, NULL)) != ESP_OK) {
        return result;
    }
    if ((result = esp_wifi_start()) != ESP_OK) return result;
    if ((result = esp_wifi_set_ps(WIFI_PS_MAX_MODEM)) != ESP_OK) return result;
    if ((result = start_http_server()) != ESP_OK) return result;

    if (station_configured) {
        ESP_LOGI(TAG, "Setup AP ready while connecting to saved Wi-Fi: open http://192.168.4.1");
    } else {
        ESP_LOGI(TAG, "Setup network ready: SSID '%s', open http://192.168.4.1", SETUP_AP_SSID);
    }
    return ESP_OK;
}

esp_err_t network_service_stop(void)
{
    esp_err_t result = ESP_OK;
    if (http_server != NULL) {
        result = httpd_stop(http_server);
        http_server = NULL;
    }
    const esp_err_t wifi_result = esp_wifi_stop();
    if (result == ESP_OK && wifi_result != ESP_OK && wifi_result != ESP_ERR_WIFI_NOT_INIT) {
        result = wifi_result;
    }
    return result;
}

network_status_t network_service_status(void)
{
    return current_status;
}

const char *network_service_status_text(void)
{
    switch (current_status) {
        case NETWORK_STATUS_SETUP: return "waiting for Wi-Fi setup";
        case NETWORK_STATUS_CONNECTING: return "connecting";
        case NETWORK_STATUS_CONNECTED: return "connected";
        case NETWORK_STATUS_CONNECTION_FAILED: return "connection failed - enter settings again";
        default: return "unknown";
    }
}

const char *network_service_ip_address(void)
{
    return station_ip;
}
