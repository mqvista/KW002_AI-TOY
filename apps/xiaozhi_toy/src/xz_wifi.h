/**
 * @file xz_wifi.h
 * @brief Wi-Fi station helpers.
 */

#ifndef __XZ_WIFI_H__
#define __XZ_WIFI_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*XZ_WIFI_LINK_CB)(BOOL_T up);

/** Initialise the Wi-Fi stack, cb is called on link up/down (may be NULL). */
OPERATE_RET xz_wifi_init(XZ_WIFI_LINK_CB cb);

/** Connect as station and wait for an IP address. */
OPERATE_RET xz_wifi_connect(const char *ssid, const char *passwd, uint32_t timeout_ms);

BOOL_T xz_wifi_is_connected(void);

/** Station MAC as "aa:bb:cc:dd:ee:ff". */
const char *xz_wifi_mac_str(void);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_WIFI_H__ */
