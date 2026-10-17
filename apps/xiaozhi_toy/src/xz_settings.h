/**
 * @file xz_settings.h
 * @brief Persistent settings (TuyaOpen KV): Wi-Fi credentials, device UUID, flags.
 */

#ifndef __XZ_SETTINGS_H__
#define __XZ_SETTINGS_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XZ_SSID_MAX   32
#define XZ_PASSWD_MAX 64
#define XZ_UUID_LEN   36

OPERATE_RET xz_settings_init(void);

/** @return TRUE when credentials are stored */
BOOL_T xz_settings_get_wifi(char ssid[XZ_SSID_MAX + 1], char passwd[XZ_PASSWD_MAX + 1]);
OPERATE_RET xz_settings_set_wifi(const char *ssid, const char *passwd);

/** Device UUID (Client-Id), generated once and kept in KV. */
const char *xz_settings_uuid(void);

/** Boolean flags kept in KV. */
BOOL_T xz_settings_get_flag(const char *key);
void xz_settings_set_flag(const char *key, BOOL_T on);

/** Integers kept in KV as decimal strings. */
int xz_settings_get_int(const char *key, int def);
void xz_settings_set_int(const char *key, int value);

#define XZ_FLAG_FORCE_PORTAL  "xz_portal"   /* long press on the reset key: start the portal after reboot */
#define XZ_FLAG_WIFI_UNTESTED "xz_wifi_new" /* credentials saved by the portal, not connected once yet */
#define XZ_KEY_VOLUME         "xz_vol"      /* speaker volume set by voice (MCP), 0..100 */

#ifdef __cplusplus
}
#endif

#endif /* __XZ_SETTINGS_H__ */
