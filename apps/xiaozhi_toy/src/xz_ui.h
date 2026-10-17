/**
 * @file xz_ui.h
 * @brief LED feedback for the device states (single LED on P8).
 */

#ifndef __XZ_UI_H__
#define __XZ_UI_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XZ_UI_OFF,        /* idle */
    XZ_UI_PORTAL,     /* fast blink: Wi-Fi configuration mode */
    XZ_UI_CONNECTING, /* slow blink: joining Wi-Fi / server */
    XZ_UI_ACTIVATING, /* medium blink: waiting for the activation code */
    XZ_UI_LISTENING,  /* steady on */
    XZ_UI_SPEAKING,   /* quick blink while the reply is played */
    XZ_UI_ERROR,      /* very fast blink */
    XZ_UI_LOW_BATTERY,/* idle, battery low: short blip every 3 s (charging has its own hardware LED) */
} XZ_UI_E;

void xz_ui_init(void);
void xz_ui_set(XZ_UI_E ui);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_UI_H__ */
