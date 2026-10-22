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
#include "photo_store.h"

#define SETUP_AP_SSID "EinkDesk-Setup"
#define SETUP_AP_PASSWORD "einkdesk"
#define MAXIMUM_RETRY_COUNT 10
#define FORM_BODY_SIZE 256
#define PHOTO_QUERY_SIZE (PHOTO_STORE_FILENAME_MAX * 3 + 16)
#define HTTP_CHUNK_SIZE 1024

static const char *TAG = "NETWORK";
static network_status_t current_status = NETWORK_STATUS_SETUP;
static char station_ip[24] = "not connected";
static bool station_configured;
static unsigned retry_count;
static httpd_handle_t http_server;
static network_todo_callbacks_t todos;
static esp_netif_t *access_point_netif;
static esp_netif_t *station_netif;

typedef struct {
    httpd_req_t *request;
    size_t count;
} photo_list_response_t;

static void note_web_activity(void)
{
    if (todos.activity != NULL) todos.activity(todos.context);
}

static const char PHOTO_UPLOAD_CONTROLS[] =
    "</ul><h3>Add images</h3>"
    "<p>Choose JPEG or PNG files. Your browser will crop and convert them for the e-paper display before upload.</p>"
    "<input id='photoFiles' type='file' accept='image/jpeg,image/png' multiple>"
    "<div id='photoQueue' class='photo-queue'></div>"
    "<button id='photoUpload' type='button'>Process and upload</button>"
    "<p id='photoStatus' class='photo-status' aria-live='polite'></p>"
    "<canvas id='photoPreview' width='480' height='800' hidden></canvas>"
    "<script>"
    "(()=>{'use strict';"
    "const W=480,H=800,FRAME=48000,files=document.getElementById('photoFiles'),queue=document.getElementById('photoQueue'),"
    "button=document.getElementById('photoUpload'),status=document.getElementById('photoStatus'),"
    "preview=document.getElementById('photoPreview'),albumList=document.getElementById('photoList'),"
    "albumSummary=document.getElementById('photoSummary');let pending=[],lastActivityPing=0;"
    "function noteActivity(){const now=Date.now();if(now-lastActivityPing<15000)return;lastActivityPing=now;"
    "fetch('/api/activity',{method:'POST'}).catch(()=>{});}"
    "document.addEventListener('input',noteActivity);document.addEventListener('click',noteActivity);"
    "function safeName(name){let safe=name.trim().replace(/[^A-Za-z0-9 ._()\\-]/g,'_').replace(/^\\.+/,'');"
    "if(!safe)safe='photo';if(safe.length<=30)return safe;const dot=safe.lastIndexOf('.');"
    "if(dot>0&&safe.length-dot<=6){const ext=safe.slice(dot);return safe.slice(0,30-ext.length)+ext;}"
    "return safe.slice(0,30);}"
    "function renderQueue(){queue.replaceChildren();for(let i=0;i<pending.length;i++){const entry=pending[i],row=document.createElement('div'),"
    "label=document.createElement('label'),input=document.createElement('input');row.className='photo-file-row';"
    "input.id='photoName'+i;input.type='text';input.maxLength=30;input.value=entry.name;"
    "label.htmlFor=input.id;label.textContent='Stored name for '+entry.file.name;"
    "input.addEventListener('input',()=>{entry.name=input.value;});input.addEventListener('blur',()=>{entry.name=safeName(entry.name);input.value=entry.name;});"
    "row.append(label,input);queue.append(row);}}"
    "files.addEventListener('change',()=>{pending=Array.from(files.files).map(file=>({file,name:safeName(file.name)}));renderQueue();status.textContent='';});"
    "function decode(file){return new Promise((resolve,reject)=>{const url=URL.createObjectURL(file),image=new Image();"
    "image.onload=()=>resolve({image,url});image.onerror=()=>{URL.revokeObjectURL(url);reject(new Error('Could not decode '+file.name));};"
    "image.src=url;});}"
    "async function convert(file){const decoded=await decode(file),image=decoded.image,ctx=preview.getContext('2d',{willReadFrequently:true});"
    "ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);const scale=Math.max(W/image.naturalWidth,H/image.naturalHeight),"
    "dw=image.naturalWidth*scale,dh=image.naturalHeight*scale;ctx.drawImage(image,(W-dw)/2,(H-dh)/2,dw,dh);"
    "URL.revokeObjectURL(decoded.url);const pixels=ctx.getImageData(0,0,W,H),data=pixels.data,gray=new Float32Array(W*H);"
    "for(let i=0,p=0;i<gray.length;i++,p+=4)gray[i]=.299*data[p]+.587*data[p+1]+.114*data[p+2];"
    "const packed=new Uint8Array(FRAME);packed.fill(255);"
    "for(let y=0;y<H;y++){for(let x=0;x<W;x++){const i=y*W+x,p=i*4,white=gray[i]>=128,q=white?255:0,error=gray[i]-q;"
    "data[p]=data[p+1]=data[p+2]=q;data[p+3]=255;if(!white){const sx=y,sy=W-1-x,bi=sy*100+(sx>>3);packed[bi]&=~(128>>(sx&7));}"
    "if(x+1<W)gray[i+1]+=error*7/16;if(y+1<H){if(x>0)gray[i+W-1]+=error*3/16;gray[i+W]+=error*5/16;"
    "if(x+1<W)gray[i+W+1]+=error/16;}}}ctx.putImageData(pixels,0,0);preview.hidden=false;return packed;}"
    "async function refreshAlbum(){const response=await fetch('/api/photos',{cache:'no-store'});if(!response.ok)return;"
    "const album=await response.json();albumList.replaceChildren();if(album.photos.length===0){const item=document.createElement('li');item.textContent='No images uploaded yet';albumList.append(item);}"
    "else for(const name of album.photos){const item=document.createElement('li'),label=document.createElement('span'),remove=document.createElement('button');"
    "label.textContent=name;remove.type='button';remove.className='photo-delete';remove.dataset.name=name;remove.textContent='Delete';item.append(label,remove);albumList.append(item);}"
    "albumSummary.textContent=album.photos.length+' image'+(album.photos.length===1?'':'s')+' · '+"
    "Math.floor(album.usedBytes/1024)+' KB used of '+Math.floor(album.totalBytes/1024)+' KB';}"
    "albumList.addEventListener('click',async event=>{const remove=event.target.closest('.photo-delete');if(!remove)return;const name=remove.dataset.name;"
    "if(!window.confirm('Delete '+name+'? This cannot be undone.'))return;remove.disabled=true;status.textContent='Deleting '+name+'...';"
    "try{const response=await fetch('/api/photos?filename='+encodeURIComponent(name),{method:'DELETE'});"
    "if(!response.ok)throw new Error((await response.text())||('Delete failed: '+response.status));await refreshAlbum();status.textContent=name+' deleted.';}"
    "catch(error){remove.disabled=false;status.textContent='Could not delete '+name+': '+error.message;}});"
    "button.addEventListener('click',async()=>{if(pending.length===0){status.textContent='Choose at least one image.';return;}"
    "let adjusted=false;for(const entry of pending){const safe=safeName(entry.name);if(safe!==entry.name)adjusted=true;entry.name=safe;}renderQueue();"
    "if(adjusted){status.textContent='Some names were adjusted to fit the device. Review them, then click upload again.';return;}"
    "const seen=new Set(),duplicate=pending.find(entry=>{const key=entry.name.toLowerCase();if(seen.has(key))return true;seen.add(key);return false;});"
    "if(duplicate){status.textContent='Each image in this batch needs a different name. Duplicate: '+duplicate.name;return;}"
    "let album;try{const response=await fetch('/api/photos',{cache:'no-store'});if(!response.ok)throw new Error('Album check failed: '+response.status);album=await response.json();}"
    "catch(error){status.textContent='Could not check for existing names: '+error.message;return;}"
    "const existing=new Set(album.photos),replacements=pending.filter(entry=>existing.has(entry.name)).map(entry=>entry.name);"
    "if(replacements.length>0&&!window.confirm('Replace existing photo'+(replacements.length===1?'':'s')+': '+replacements.join(', ')+'?'))return;"
    "button.disabled=true;let uploaded=0;const failures=[],retry=[];const total=pending.length;for(let i=0;i<pending.length;i++){const entry=pending[i],file=entry.file,name=entry.name;"
    "try{status.textContent='Processing '+(i+1)+' of '+total+': '+file.name;const frame=await convert(file);"
    "status.textContent='Uploading '+(i+1)+' of '+total+': '+name;const response=await fetch('/api/photos?filename='+encodeURIComponent(name),"
    "{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:frame});if(!response.ok)throw new Error((await response.text())||('Upload failed: '+response.status));uploaded++;}"
    "catch(error){failures.push(file.name+': '+error.message);retry.push(entry);}}"
    "try{await refreshAlbum();}catch(error){}button.disabled=false;pending=retry;if(pending.length===0)files.value='';renderQueue();"
    "status.textContent=failures.length===0?uploaded+' image'+(uploaded===1?'':'s')+' uploaded successfully.':"
    "uploaded+' uploaded; '+failures.length+' failed. '+failures.join(' | ');});"
    "})();"
    "</script></main></body></html>";

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

static esp_err_t photo_filename_from_request(
    httpd_req_t *request,
    char filename[PHOTO_STORE_FILENAME_MAX + 1])
{
    char query[PHOTO_QUERY_SIZE] = {0};
    char encoded[PHOTO_STORE_FILENAME_MAX * 3 + 1] = {0};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "filename", encoded, sizeof(encoded)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    url_decode(encoded);
    if (!photo_store_filename_is_valid(encoded)) return ESP_ERR_INVALID_ARG;
    strlcpy(filename, encoded, PHOTO_STORE_FILENAME_MAX + 1);
    return ESP_OK;
}

static esp_err_t photo_html_list_item(const char *filename, void *context)
{
    photo_list_response_t *response = context;
    char escaped[PHOTO_STORE_FILENAME_MAX * 6 + 1];
    char item[sizeof(escaped) * 2 + 128];
    html_escape(filename, escaped, sizeof(escaped));
    const int length = snprintf(
        item,
        sizeof(item),
        "<li><span>%s</span><button type='button' class='photo-delete' data-name='%s'>Delete</button></li>",
        escaped,
        escaped);
    if (length < 0 || length >= sizeof(item)) return ESP_FAIL;
    const esp_err_t result = httpd_resp_send_chunk(response->request, item, length);
    if (result == ESP_OK) ++response->count;
    return result;
}

static esp_err_t photo_json_list_item(const char *filename, void *context)
{
    photo_list_response_t *response = context;
    char item[PHOTO_STORE_FILENAME_MAX + 5];
    const int length = snprintf(item, sizeof(item), "%s\"%s\"", response->count == 0 ? "" : ",", filename);
    if (length < 0 || length >= sizeof(item)) return ESP_FAIL;
    const esp_err_t result = httpd_resp_send_chunk(response->request, item, length);
    if (result == ESP_OK) ++response->count;
    return result;
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    note_web_activity();
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
        ".done{color:#555}li{padding:4px 0}.photo-status{min-height:24px;font-weight:700}"
        ".photo-queue{margin-top:16px}.photo-file-row{padding:12px;margin-top:10px;background:#eee;border-left:4px solid #171717}"
        ".photo-file-row label{margin-top:0;font-size:14px}.photo-delete{margin:0 0 0 12px;padding:6px 10px;font-size:13px}"
        "canvas{display:block;width:240px;max-width:100%%;height:auto;margin:20px auto;border:2px solid #171717}"
        "button:disabled{opacity:.55}</style></head><body><main>"
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
        "<button type='submit'>Save and connect</button></form>",
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
    esp_err_t result = httpd_resp_send_chunk(request, page, length);
    if (result != ESP_OK) return result;

    photo_store_info_t photo_info = {0};
    result = photo_store_get_info(&photo_info);
    if (result != ESP_OK) {
        result = httpd_resp_send_chunk(request,
            "<hr><h2>Photo album</h2><p>Photo storage is unavailable.</p></main></body></html>",
            HTTPD_RESP_USE_STRLEN);
        if (result != ESP_OK) return result;
        return httpd_resp_send_chunk(request, NULL, 0);
    }

    char album_header[256];
    const int album_header_length = snprintf(
        album_header,
        sizeof(album_header),
        "<hr><h2>Photo album</h2><p id='photoSummary'>%u image%s &middot; %u KB used of %u KB</p>"
        "<ul id='photoList'>",
        (unsigned)photo_info.image_count,
        photo_info.image_count == 1 ? "" : "s",
        (unsigned)(photo_info.used_bytes / 1024U),
        (unsigned)(photo_info.total_bytes / 1024U));
    if (album_header_length < 0 || album_header_length >= sizeof(album_header)) return ESP_FAIL;
    result = httpd_resp_send_chunk(request, album_header, album_header_length);
    if (result != ESP_OK) return result;

    photo_list_response_t response = {.request = request};
    result = photo_store_list(photo_html_list_item, &response);
    if (result != ESP_OK) return result;
    if (response.count == 0) {
        result = httpd_resp_send_chunk(request, "<li>No images uploaded yet</li>", HTTPD_RESP_USE_STRLEN);
        if (result != ESP_OK) return result;
    }
    result = httpd_resp_send_chunk(
        request,
        PHOTO_UPLOAD_CONTROLS,
        HTTPD_RESP_USE_STRLEN);
    if (result != ESP_OK) return result;
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t photos_api_get_handler(httpd_req_t *request)
{
    note_web_activity();
    photo_store_info_t info = {0};
    esp_err_t result = photo_store_get_info(&info);
    if (result != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Photo storage unavailable");
    }

    httpd_resp_set_type(request, "application/json");
    result = httpd_resp_send_chunk(request, "{\"photos\":[", HTTPD_RESP_USE_STRLEN);
    if (result != ESP_OK) return result;
    photo_list_response_t response = {.request = request};
    result = photo_store_list(photo_json_list_item, &response);
    if (result != ESP_OK) return result;

    char summary[160];
    const int length = snprintf(
        summary,
        sizeof(summary),
        "],\"imageSize\":%u,\"usedBytes\":%u,\"totalBytes\":%u}",
        (unsigned)PHOTO_STORE_IMAGE_SIZE,
        (unsigned)info.used_bytes,
        (unsigned)info.total_bytes);
    if (length < 0 || length >= sizeof(summary)) return ESP_FAIL;
    result = httpd_resp_send_chunk(request, summary, length);
    if (result != ESP_OK) return result;
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t photos_api_post_handler(httpd_req_t *request)
{
    note_web_activity();
    if (request->content_len != PHOTO_STORE_IMAGE_SIZE) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "A photo must be exactly 48000 bytes");
    }

    char filename[PHOTO_STORE_FILENAME_MAX + 1] = {0};
    if (photo_filename_from_request(request, filename) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "A filename query parameter is required");
    }

    photo_store_writer_t *writer = NULL;
    esp_err_t result = photo_store_write_begin(filename, &writer);
    if (result == ESP_ERR_NO_MEM) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough photo storage space");
    }
    if (result != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not begin photo upload");
    }

    uint8_t chunk[HTTP_CHUNK_SIZE];
    size_t remaining = request->content_len;
    while (remaining > 0) {
        const size_t wanted = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        const int received = httpd_req_recv(request, (char *)chunk, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0) {
            photo_store_write_abort(writer);
            return ESP_FAIL;
        }
        note_web_activity();
        result = photo_store_write(writer, chunk, (size_t)received);
        if (result != ESP_OK) {
            photo_store_write_abort(writer);
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save photo data");
        }
        remaining -= (size_t)received;
    }

    result = photo_store_write_finish(writer);
    if (result != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not commit photo");
    }

    char response[PHOTO_STORE_FILENAME_MAX + 32];
    const int length = snprintf(response, sizeof(response), "{\"filename\":\"%s\"}", filename);
    if (length < 0 || length >= sizeof(response)) return ESP_FAIL;
    httpd_resp_set_status(request, "201 Created");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, length);
}

static esp_err_t photos_api_delete_handler(httpd_req_t *request)
{
    note_web_activity();
    char filename[PHOTO_STORE_FILENAME_MAX + 1] = {0};
    if (photo_filename_from_request(request, filename) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "A valid filename query parameter is required");
    }

    const esp_err_t result = photo_store_delete(filename);
    if (result == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Photo not found");
    }
    if (result != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not delete photo");
    }

    char response[PHOTO_STORE_FILENAME_MAX + 32];
    const int length = snprintf(response, sizeof(response), "{\"deleted\":\"%s\"}", filename);
    if (length < 0 || length >= sizeof(response)) return ESP_FAIL;
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, length);
}

static esp_err_t todos_post_handler(httpd_req_t *request)
{
    note_web_activity();
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
    note_web_activity();
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
    note_web_activity();
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

static esp_err_t activity_api_post_handler(httpd_req_t *request)
{
    note_web_activity();
    return httpd_resp_sendstr(request, "");
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
    const httpd_uri_t photos_api_get_uri = {
        .uri = "/api/photos",
        .method = HTTP_GET,
        .handler = photos_api_get_handler,
    };
    const httpd_uri_t photos_api_post_uri = {
        .uri = "/api/photos",
        .method = HTTP_POST,
        .handler = photos_api_post_handler,
    };
    const httpd_uri_t photos_api_delete_uri = {
        .uri = "/api/photos",
        .method = HTTP_DELETE,
        .handler = photos_api_delete_handler,
    };
    const httpd_uri_t activity_api_uri = {
        .uri = "/api/activity",
        .method = HTTP_POST,
        .handler = activity_api_post_handler,
    };
    result = httpd_register_uri_handler(http_server, &root_uri);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(http_server, &wifi_uri);
    }
    if (result == ESP_OK) result = httpd_register_uri_handler(http_server, &todos_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(http_server, &todos_api_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(http_server, &photos_api_get_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(http_server, &photos_api_post_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(http_server, &photos_api_delete_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(http_server, &activity_api_uri);
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
