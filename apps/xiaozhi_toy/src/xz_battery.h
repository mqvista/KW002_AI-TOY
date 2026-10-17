/**
 * @file xz_battery.h
 * @brief Battery level and charger state (board power driver: P28 ADC, P21 CHRG).
 */

#ifndef __XZ_BATTERY_H__
#define __XZ_BATTERY_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t mv;       /* averaged cell voltage */
    uint8_t level;     /* 0..100 from the original firmware's OCV table */
    BOOL_T charging;   /* P21 low; the charger has no "full" line */
} XZ_BATTERY_T;

/** Called from the power driver thread when the charger starts or stops. */
typedef void (*XZ_BATTERY_CHG_CB)(BOOL_T charging);

OPERATE_RET xz_battery_init(XZ_BATTERY_CHG_CB cb);

/**
 * Call regularly from the main loop. speaker_busy: audio is being played; samples are
 * only taken after the speaker was quiet for a while (it pulls the cell down).
 */
void xz_battery_poll(BOOL_T speaker_busy);

/** @return FALSE until the first sample was taken (charging is always filled in) */
BOOL_T xz_battery_get(XZ_BATTERY_T *out);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_BATTERY_H__ */
