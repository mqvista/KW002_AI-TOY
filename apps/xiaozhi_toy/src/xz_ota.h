/**
 * @file xz_ota.h
 * @brief XiaoZhi device check-in ("OTA" endpoint) and activation (protocol v1).
 */

#ifndef __XZ_OTA_H__
#define __XZ_OTA_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ws_url[160];
    char ws_token[160];
    BOOL_T has_activation;
    char act_code[16];
    int64_t server_time_ms;   /* 0 when absent */
    int tz_offset_min;
} XZ_OTA_INFO_T;

/** POST the device info to the OTA endpoint and parse the answer. */
OPERATE_RET xz_ota_check(XZ_OTA_INFO_T *info);

/**
 * @brief Ask the server whether the activation code has been entered.
 * @return OPRT_OK when activated, OPRT_RESOURCE_NOT_READY while still pending.
 */
OPERATE_RET xz_ota_activate(void);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_OTA_H__ */
