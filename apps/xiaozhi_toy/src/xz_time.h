/**
 * @file xz_time.h
 * @brief Wall clock sync (SNTP). TLS certificate checks need a sane date.
 */

#ifndef __XZ_TIME_H__
#define __XZ_TIME_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Query public NTP servers and set the system clock. */
OPERATE_RET xz_time_sync(void);

/** Set the system clock from a UNIX time in ms (e.g. the OTA server_time). */
void xz_time_set_ms(int64_t unix_ms);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_TIME_H__ */
