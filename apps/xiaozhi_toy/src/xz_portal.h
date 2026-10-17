/**
 * @file xz_portal.h
 * @brief SoftAP Wi-Fi provisioning portal (open AP + captive web page).
 */

#ifndef __XZ_PORTAL_H__
#define __XZ_PORTAL_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Scan nearby networks, start the "Xiaozhi-XXXX" SoftAP, the web page and the
 *        captive DNS, then return; the services keep running in their own threads.
 *        When the user submits a network the credentials are saved and the device
 *        reboots into station mode.
 */
OPERATE_RET xz_portal_run(void);

/** SSID of the provisioning AP ("Xiaozhi-5C90"), valid once xz_portal_run() was called. */
const char *xz_portal_ssid(void);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_PORTAL_H__ */
