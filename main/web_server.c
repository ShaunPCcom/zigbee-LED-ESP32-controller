// SPDX-License-Identifier: MIT
/**
 * LED Controller web server — thin wrapper over web_server_base.
 *
 * Handles only LED-specific endpoints:
 *   GET  /api/config      — strip + transition config JSON
 *   POST /api/config      — strip config update
 *   GET  /api/segments    — all 8 segment states JSON
 *   POST /api/segments    — update one or more segments
 *   GET  /api/presets     — preset slot list
 *   POST /api/presets/apply  — recall a preset
 *   POST /api/presets/save   — save current state to a slot
 *   POST /api/presets/delete — delete a slot
 *
 * All WiFi, OTA, system, and diagnostics endpoints are handled by
 * web_server_base and registered automatically in web_server_base_start().
 */
#include "web_server.h"
#include "web_server_base.h"

#include "config_api.h"
#include "version.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_server";

#define MAX_BODY_LEN 4096

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");
extern const char style_css_start[]  asm("_binary_style_css_start");
extern const char style_css_end[]    asm("_binary_style_css_end");

/* ========================================================================== */
/*  Helpers                                                                    */
/* ========================================================================== */

static char *read_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY_LEN) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - (size_t)received);
        if (r <= 0) { free(buf); return NULL; }
        received += r;
    }
    buf[received] = '\0';
    return buf;
}

static void send_json(httpd_req_t *req, int status, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (status == 400) httpd_resp_set_status(req, "400 Bad Request");
    else if (status == 404) httpd_resp_set_status(req, "404 Not Found");
    else if (status == 500) httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, str ? str : "{}");
    free(str);
}

static void send_ok(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    send_json(req, 200, r);
    cJSON_Delete(r);
}

static void send_error(httpd_req_t *req, int status, const char *msg)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", msg);
    send_json(req, status, e);
    cJSON_Delete(e);
}

/* ========================================================================== */
/*  GET /api/config                                                            */
/* ========================================================================== */

static esp_err_t handle_get_config(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (config_api_get_strip_config(&json) != ESP_OK) {
        send_error(req, 500, "Failed to read strip config");
        return ESP_OK;
    }
    send_json(req, 200, json);
    cJSON_Delete(json);
    return ESP_OK;
}

/* ========================================================================== */
/*  POST /api/config                                                           */
/* ========================================================================== */

static esp_err_t handle_post_config(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { send_error(req, 400, "No body or too large (max 4096)"); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { send_error(req, 400, "Invalid JSON"); return ESP_OK; }

    esp_err_t err = config_api_set_strip_config(root);
    cJSON_Delete(root);

    if (err != ESP_OK) { send_error(req, 500, "Failed to apply config"); return ESP_OK; }
    send_ok(req);
    return ESP_OK;
}

/* ========================================================================== */
/*  GET /api/segments                                                          */
/* ========================================================================== */

static esp_err_t handle_get_segments(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (config_api_get_segments(&json) != ESP_OK) {
        send_error(req, 500, "Failed to read segments");
        return ESP_OK;
    }
    send_json(req, 200, json);
    cJSON_Delete(json);
    return ESP_OK;
}

/* ========================================================================== */
/*  POST /api/segments                                                         */
/* ========================================================================== */

static esp_err_t handle_post_segments(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { send_error(req, 400, "No body or too large (max 4096)"); return ESP_OK; }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { send_error(req, 400, "Invalid JSON"); return ESP_OK; }

    esp_err_t err = config_api_set_segments(root);
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG) { send_error(req, 400, "Missing or invalid index"); return ESP_OK; }
    if (err != ESP_OK)              { send_error(req, 500, "Failed to apply segments"); return ESP_OK; }
    send_ok(req);
    return ESP_OK;
}

/* ========================================================================== */
/*  GET /api/presets                                                           */
/* ========================================================================== */

static esp_err_t handle_get_presets(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (config_api_get_presets(&json) != ESP_OK) {
        send_error(req, 500, "Failed to read presets");
        return ESP_OK;
    }
    send_json(req, 200, json);
    cJSON_Delete(json);
    return ESP_OK;
}

/* ========================================================================== */
/*  POST /api/presets/apply                                                    */
/* ========================================================================== */

static esp_err_t handle_apply_preset(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { send_error(req, 400, "No body"); return ESP_OK; }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { send_error(req, 400, "Invalid JSON"); return ESP_OK; }

    cJSON *slot_item = cJSON_GetObjectItem(root, "slot");
    if (!slot_item || !cJSON_IsNumber(slot_item)) {
        cJSON_Delete(root);
        send_error(req, 400, "Missing 'slot'");
        return ESP_OK;
    }
    int slot = slot_item->valueint;
    cJSON_Delete(root);

    esp_err_t err = config_api_apply_preset(slot);
    if (err == ESP_ERR_NOT_FOUND)    { send_error(req, 404, "Slot empty"); return ESP_OK; }
    if (err == ESP_ERR_INVALID_ARG)  { send_error(req, 400, "Invalid slot"); return ESP_OK; }
    if (err != ESP_OK)               { send_error(req, 500, "Apply failed"); return ESP_OK; }
    send_ok(req);
    return ESP_OK;
}

/* ========================================================================== */
/*  POST /api/presets/save                                                     */
/* ========================================================================== */

static esp_err_t handle_save_preset(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { send_error(req, 400, "No body"); return ESP_OK; }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { send_error(req, 400, "Invalid JSON"); return ESP_OK; }

    cJSON *slot_item = cJSON_GetObjectItem(root, "slot");
    if (!slot_item || !cJSON_IsNumber(slot_item)) {
        cJSON_Delete(root);
        send_error(req, 400, "Missing 'slot'");
        return ESP_OK;
    }
    int slot = slot_item->valueint;
    cJSON *name_item = cJSON_GetObjectItem(root, "name");
    const char *name = (name_item && cJSON_IsString(name_item)) ? name_item->valuestring : NULL;

    esp_err_t err = config_api_save_preset(slot, name);
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG) { send_error(req, 400, "Invalid slot"); return ESP_OK; }
    if (err != ESP_OK)              { send_error(req, 500, "Save failed"); return ESP_OK; }
    send_ok(req);
    return ESP_OK;
}

/* ========================================================================== */
/*  POST /api/presets/delete                                                   */
/* ========================================================================== */

static esp_err_t handle_delete_preset(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) { send_error(req, 400, "No body"); return ESP_OK; }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { send_error(req, 400, "Invalid JSON"); return ESP_OK; }

    cJSON *slot_item = cJSON_GetObjectItem(root, "slot");
    if (!slot_item || !cJSON_IsNumber(slot_item)) {
        cJSON_Delete(root);
        send_error(req, 400, "Missing 'slot'");
        return ESP_OK;
    }
    int slot = slot_item->valueint;
    cJSON_Delete(root);

    esp_err_t err = config_api_delete_preset(slot);
    if (err == ESP_ERR_INVALID_ARG) { send_error(req, 400, "Invalid slot"); return ESP_OK; }
    if (err != ESP_OK)              { send_error(req, 500, "Delete failed"); return ESP_OK; }
    send_ok(req);
    return ESP_OK;
}

/* ========================================================================== */
/*  Public API                                                                 */
/* ========================================================================== */

esp_err_t web_server_start(void)
{
    web_server_base_config_t cfg = {
        .device_name       = "LED Controller",
        .firmware_version  = FIRMWARE_VERSION_STRING,
        .nvs_namespace     = "led_cfg",
        .ota_image_type      = 0x0004,   /* LED-C6 */
        .current_version_hex = FIRMWARE_VERSION,
        .index_html_start  = (const uint8_t *)index_html_start,
        .index_html_size   = (size_t)(index_html_end - index_html_start),
        .app_js_start      = (const uint8_t *)app_js_start,
        .app_js_size       = (size_t)(app_js_end - app_js_start),
        .style_css_start   = (const uint8_t *)style_css_start,
        .style_css_size    = (size_t)(style_css_end - style_css_start),
    };

    esp_err_t err = web_server_base_start(&cfg);
    if (err != ESP_OK) return err;

    web_server_base_register("/api/config",          HTTP_GET,  handle_get_config,     false);
    web_server_base_register("/api/config",          HTTP_POST, handle_post_config,    false);
    web_server_base_register("/api/segments",        HTTP_GET,  handle_get_segments,   false);
    web_server_base_register("/api/segments",        HTTP_POST, handle_post_segments,  false);
    web_server_base_register("/api/presets",         HTTP_GET,  handle_get_presets,    false);
    web_server_base_register("/api/presets/apply",   HTTP_POST, handle_apply_preset,   false);
    web_server_base_register("/api/presets/save",    HTTP_POST, handle_save_preset,    false);
    web_server_base_register("/api/presets/delete",  HTTP_POST, handle_delete_preset,  false);

    ESP_LOGI(TAG, "LED web server started");
    return ESP_OK;
}

void web_server_stop(void)
{
    web_server_base_stop();
}
