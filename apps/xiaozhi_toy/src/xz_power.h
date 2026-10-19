/** Standby policy. Call poll/enter from the application thread. */
#ifndef __XZ_POWER_H__
#define __XZ_POWER_H__
#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

OPERATE_RET xz_power_init(void);
/** Thread-safe: key/KWS callbacks invalidate a sleep attempt immediately. */
void xz_power_activity(void);
/** Idle states only; playing, charging and held keys restart the idle interval. */
BOOL_T xz_power_should_sleep(BOOL_T idle);
/** Recheck activity and GPIOs, arm P33, then enter reboot-based deep sleep. */
OPERATE_RET xz_power_enter_sleep(void);
/** Rate-limit failed preparation; does not pretend an attempt was successful. */
void xz_power_defer(void);

#ifdef __cplusplus
}
#endif
#endif
