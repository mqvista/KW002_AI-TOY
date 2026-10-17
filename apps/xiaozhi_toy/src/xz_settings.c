/**
 * @file xz_settings.c
 * @brief Persistent settings (TuyaOpen KV): Wi-Fi credentials, device UUID, flags.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "tal_api.h"
#include "tal_kv.h"

#include "xz_settings.h"

#define KV_WIFI_SSID   "xz_ssid"
#define KV_WIFI_PASSWD "xz_pwd"
#define KV_UUID        "xz_uuid"

static char sg_uuid[XZ_UUID_LEN + 1] = {0};

/* Read a KV string into buf (NUL terminated). */
static BOOL_T __kv_get_str(const char *key, char *buf, size_t buf_len)
{
    uint8_t *value = NULL;
    size_t len = 0;

    buf[0] = '\0';
    if (OPRT_OK != tal_kv_get(key, &value, &len) || NULL == value) {
        return FALSE;
    }
    if (len >= buf_len) {
        len = buf_len - 1;
    }
    memcpy(buf, value, len);
    buf[len] = '\0';
    tal_kv_free(value);
    return TRUE;
}

static void __uuid_generate(char out[XZ_UUID_LEN + 1])
{
    uint8_t b[16];

    for (int i = 0; i < 16; i++) {
        b[i] = (uint8_t)tal_system_get_random(0xFF);
    }
    b[6] = (b[6] & 0x0F) | 0x40; /* version 4 */
    b[8] = (b[8] & 0x3F) | 0x80; /* variant */
    snprintf(out, XZ_UUID_LEN + 1, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
             b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

OPERATE_RET xz_settings_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    TUYA_CALL_ERR_RETURN(tal_kv_init(&(tal_kv_cfg_t){
        .seed = "kw002xiaozhiseed",
        .key = "kw002xiaozhi_key",
    }));

    if (!__kv_get_str(KV_UUID, sg_uuid, sizeof(sg_uuid)) || strlen(sg_uuid) != XZ_UUID_LEN) {
        __uuid_generate(sg_uuid);
        tal_kv_set(KV_UUID, (const uint8_t *)sg_uuid, strlen(sg_uuid));
        PR_NOTICE("generated device uuid %s", sg_uuid);
    }

    return rt;
}

BOOL_T xz_settings_get_wifi(char ssid[XZ_SSID_MAX + 1], char passwd[XZ_PASSWD_MAX + 1])
{
    if (!__kv_get_str(KV_WIFI_SSID, ssid, XZ_SSID_MAX + 1) || ssid[0] == '\0') {
        return FALSE;
    }
    __kv_get_str(KV_WIFI_PASSWD, passwd, XZ_PASSWD_MAX + 1); /* empty for open networks */
    return TRUE;
}

OPERATE_RET xz_settings_set_wifi(const char *ssid, const char *passwd)
{
    OPERATE_RET rt = OPRT_OK;

    TUYA_CALL_ERR_RETURN(tal_kv_set(KV_WIFI_SSID, (const uint8_t *)ssid, strlen(ssid)));
    /* tal_kv does not take zero length values, store a single NUL for open networks */
    if (passwd[0]) {
        TUYA_CALL_ERR_RETURN(tal_kv_set(KV_WIFI_PASSWD, (const uint8_t *)passwd, strlen(passwd)));
    } else {
        TUYA_CALL_ERR_RETURN(tal_kv_set(KV_WIFI_PASSWD, (const uint8_t *)"", 1));
    }
    return rt;
}

const char *xz_settings_uuid(void)
{
    return sg_uuid;
}

BOOL_T xz_settings_get_flag(const char *key)
{
    char v[4];
    return __kv_get_str(key, v, sizeof(v)) && v[0] == '1';
}

int xz_settings_get_int(const char *key, int def)
{
    char v[12];
    return __kv_get_str(key, v, sizeof(v)) && v[0] ? atoi(v) : def;
}

void xz_settings_set_int(const char *key, int value)
{
    char v[12];
    snprintf(v, sizeof(v), "%d", value);
    tal_kv_set(key, (const uint8_t *)v, strlen(v));
}

void xz_settings_set_flag(const char *key, BOOL_T on)
{
    if (on) {
        tal_kv_set(key, (const uint8_t *)"1", 1);
    } else {
        tal_kv_del(key);
    }
}
