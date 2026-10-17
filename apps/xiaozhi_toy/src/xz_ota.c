/**
 * @file xz_ota.c
 * @brief XiaoZhi device check-in ("OTA" endpoint) and activation (protocol v1).
 *
 * Same requests as 78/xiaozhi-esp32 main/ota.cc without a serial number:
 *   POST /xiaozhi/ota/          system info JSON -> websocket url/token, activation code
 *   POST /xiaozhi/ota/activate  "{}" -> 202 while waiting for the user, 200 once bound
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "cJSON.h"
#include "http_client_interface.h"

#include "xz_certs.h"
#include "xz_config.h"
#include "xz_ota.h"
#include "xz_settings.h"
#include "xz_wifi.h"

#define OTA_TIMEOUT_MS 15000

static char sg_user_agent[48];

static void __fill_headers(http_client_header_t hdr[6])
{
    snprintf(sg_user_agent, sizeof(sg_user_agent), "%s/%s", XZ_BOARD_TYPE, PROJECT_VERSION);
    hdr[0] = (http_client_header_t){"Activation-Version", "1"};
    hdr[1] = (http_client_header_t){"Device-Id", xz_wifi_mac_str()};
    hdr[2] = (http_client_header_t){"Client-Id", xz_settings_uuid()};
    hdr[3] = (http_client_header_t){"User-Agent", sg_user_agent};
    hdr[4] = (http_client_header_t){"Accept-Language", XZ_LANGUAGE};
    hdr[5] = (http_client_header_t){"Content-Type", "application/json"};
}

static OPERATE_RET __post(const char *path, const char *body, uint16_t *status, char **resp_body)
{
    http_client_header_t hdr[6];
    http_client_response_t resp = {0};
    http_client_status_t st;

    __fill_headers(hdr);
    st = http_client_request(
        &(const http_client_request_t){
            .host = XZ_OTA_HOST,
            .port = XZ_OTA_PORT,
            .path = path,
            .cacert = (const uint8_t *)g_xz_root_ca_pem,
            .cacert_len = strlen(g_xz_root_ca_pem) + 1, /* mbedtls PEM parser wants the NUL */
            .method = "POST",
            .headers = hdr,
            .headers_count = 6,
            .body = (const uint8_t *)body,
            .body_length = strlen(body),
            .timeout_ms = OTA_TIMEOUT_MS,
        },
        &resp);
    if (HTTP_CLIENT_SUCCESS != st) {
        PR_ERR("POST %s failed: %d", path, st);
        return OPRT_COM_ERROR;
    }

    *status = resp.status_code;
    if (resp_body) {
        *resp_body = tal_malloc(resp.body_length + 1);
        if (*resp_body) {
            memcpy(*resp_body, resp.body, resp.body_length);
            (*resp_body)[resp.body_length] = '\0';
        }
    }
    http_client_free(&resp);
    return OPRT_OK;
}

static char *__system_info_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *chip = cJSON_CreateObject();
    cJSON *app = cJSON_CreateObject();
    cJSON *board = cJSON_CreateObject();
    char compile_time[32];
    char *json = NULL;

    snprintf(compile_time, sizeof(compile_time), "%sT%sZ", __DATE__, __TIME__);

    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", XZ_LANGUAGE);
    cJSON_AddNumberToObject(root, "flash_size", 8 * 1024 * 1024);
    cJSON_AddStringToObject(root, "minimum_free_heap_size", "0");
    cJSON_AddStringToObject(root, "mac_address", xz_wifi_mac_str());
    cJSON_AddStringToObject(root, "uuid", xz_settings_uuid());
    cJSON_AddStringToObject(root, "chip_model_name", XZ_CHIP_MODEL);

    cJSON_AddNumberToObject(chip, "model", 7258);
    cJSON_AddNumberToObject(chip, "cores", 2);
    cJSON_AddNumberToObject(chip, "revision", 0);
    cJSON_AddNumberToObject(chip, "features", 0);
    cJSON_AddItemToObject(root, "chip_info", chip);

    cJSON_AddStringToObject(app, "name", XZ_APP_NAME);
    cJSON_AddStringToObject(app, "version", PROJECT_VERSION);
    cJSON_AddStringToObject(app, "compile_time", compile_time);
    cJSON_AddStringToObject(app, "idf_version", "tuyaopen-" OPEN_VERSION);
    cJSON_AddStringToObject(app, "elf_sha256", "0000000000000000000000000000000000000000000000000000000000000000");
    cJSON_AddItemToObject(root, "application", app);

    cJSON_AddItemToObject(root, "partition_table", cJSON_CreateArray());
    cJSON *ota = cJSON_CreateObject();
    cJSON_AddStringToObject(ota, "label", "app");
    cJSON_AddItemToObject(root, "ota", ota);

    cJSON_AddStringToObject(board, "type", XZ_BOARD_TYPE);
    cJSON_AddStringToObject(board, "name", XZ_BOARD_TYPE);
    cJSON_AddStringToObject(board, "mac", xz_wifi_mac_str());
    cJSON_AddItemToObject(root, "board", board);

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void __copy_str(cJSON *obj, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(out, out_len, "%s", item->valuestring);
    }
}

OPERATE_RET xz_ota_check(XZ_OTA_INFO_T *info)
{
    OPERATE_RET rt = OPRT_OK;
    uint16_t status = 0;
    char *body = NULL;
    char *req = __system_info_json();
    cJSON *root = NULL;

    memset(info, 0, sizeof(*info));
    if (NULL == req) {
        return OPRT_MALLOC_FAILED;
    }
    rt = __post(XZ_OTA_PATH, req, &status, &body);
    cJSON_free(req);
    if (OPRT_OK != rt) {
        return rt;
    }
    if (200 != status || NULL == body) {
        PR_ERR("ota check http %u: %s", status, body ? body : "");
        tal_free(body);
        return OPRT_COM_ERROR;
    }

    root = cJSON_Parse(body);
    tal_free(body);
    if (NULL == root) {
        PR_ERR("ota check: bad json");
        return OPRT_CJSON_PARSE_ERR;
    }

    cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(ws)) {
        __copy_str(ws, "url", info->ws_url, sizeof(info->ws_url));
        __copy_str(ws, "token", info->ws_token, sizeof(info->ws_token));
    }

    cJSON *act = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(act)) {
        __copy_str(act, "code", info->act_code, sizeof(info->act_code));
        info->has_activation = info->act_code[0] != '\0';
    }

    cJSON *st = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(st)) {
        cJSON *ts = cJSON_GetObjectItem(st, "timestamp");
        cJSON *tz = cJSON_GetObjectItem(st, "timezone_offset");
        if (cJSON_IsNumber(ts)) {
            info->server_time_ms = (int64_t)ts->valuedouble;
        }
        if (cJSON_IsNumber(tz)) {
            info->tz_offset_min = tz->valueint;
        }
    }
    cJSON_Delete(root);

    PR_NOTICE("ota: websocket %s, activation %s", info->ws_url[0] ? info->ws_url : "(none)",
              info->has_activation ? info->act_code : "done");
    return info->ws_url[0] ? OPRT_OK : OPRT_NOT_FOUND;
}

OPERATE_RET xz_ota_activate(void)
{
    uint16_t status = 0;
    char *body = NULL;
    OPERATE_RET rt = __post(XZ_OTA_ACTIVATE_PATH, "{}", &status, &body);

    if (OPRT_OK != rt) {
        return rt;
    }
    PR_DEBUG("activate: http %u %s", status, body ? body : "");
    tal_free(body);
    if (200 == status) {
        return OPRT_OK;
    }
    if (202 == status) {
        return OPRT_RESOURCE_NOT_READY;
    }
    return OPRT_COM_ERROR;
}
