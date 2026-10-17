/**
 * @file xz_ui.c
 * @brief LED feedback for the device states (single LED on P8).
 */

#include "tal_api.h"
#include "tdl_led_manage.h"

#include "xz_ui.h"

static TDL_LED_HANDLE_T sg_led = NULL;
static XZ_UI_E sg_ui = XZ_UI_OFF;

void xz_ui_init(void)
{
    sg_led = tdl_led_find_dev(LED_NAME);
    if (NULL == sg_led || OPRT_OK != tdl_led_open(sg_led)) {
        PR_ERR("led open failed");
        sg_led = NULL;
        return;
    }
    tdl_led_set_status(sg_led, TDL_LED_OFF);
}

void xz_ui_set(XZ_UI_E ui)
{
    if (NULL == sg_led || ui == sg_ui) {
        return;
    }
    sg_ui = ui;

    switch (ui) {
    case XZ_UI_PORTAL:
        tdl_led_flash(sg_led, 150);
        break;
    case XZ_UI_CONNECTING:
        tdl_led_flash(sg_led, 600);
        break;
    case XZ_UI_ACTIVATING:
        tdl_led_flash(sg_led, 300);
        break;
    case XZ_UI_LISTENING:
        tdl_led_set_status(sg_led, TDL_LED_ON);
        break;
    case XZ_UI_SPEAKING:
        tdl_led_flash(sg_led, 250);
        break;
    case XZ_UI_ERROR:
        tdl_led_flash(sg_led, 80);
        break;
    case XZ_UI_LOW_BATTERY: {
        TDL_LED_BLINK_CFG_T blink = {.cnt = TDL_BLINK_FOREVER,
                                     .start_stat = TDL_LED_ON,
                                     .end_stat = TDL_LED_OFF,
                                     .first_half_cycle_time = 100,
                                     .latter_half_cycle_time = 2900};
        tdl_led_blink(sg_led, &blink);
        break;
    }
    case XZ_UI_OFF:
    default:
        tdl_led_set_status(sg_led, TDL_LED_OFF);
        break;
    }
}
