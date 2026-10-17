/**
 * @file board_com_api.h
 * @brief KW002 AI toy (Tuya T5AI module) board-level hardware registration APIs.
 *
 * Pin map recovered from the original firmware (tuyaos_demo_ai_toy 1.0.31),
 * see docs/firmware_io_analysis.md.
 */

#ifndef __BOARD_COM_API_H__
#define __BOARD_COM_API_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/
#define BOARD_SPEAKER_EN_PIN     TUYA_GPIO_NUM_19 /* PA enable, high = on */

#define BOARD_BUTTON_TALK_PIN    TUYA_GPIO_NUM_33 /* chat key, active low, wakeup source */
#define BOARD_BUTTON_RESET_PIN   TUYA_GPIO_NUM_12 /* reset / provisioning key, active low */
#define BOARD_BUTTON_ACTIVE_LV   TUYA_GPIO_LEVEL_LOW

#define BOARD_LED_PIN            TUYA_GPIO_NUM_8
#define BOARD_LED_ACTIVE_LV      TUYA_GPIO_LEVEL_HIGH /* TODO: verify on hardware */

#define BOARD_CHRG_PIN           TUYA_GPIO_NUM_21 /* charger CHRG, low = charging */
#define BOARD_BAT_ADC_PIN        TUYA_GPIO_NUM_28 /* SARADC channel 4 */
#define BOARD_BAT_ADC_CH         4
#define BOARD_BAT_DIVIDER        4.0f /* VBAT = V_adc * 4 (original fw), TODO: verify */

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Registers all the hardware peripherals (audio, buttons, LED, power) on the board.
 *
 * @return Returns OPERATE_RET_OK on success, or an appropriate error code on failure.
 */
OPERATE_RET board_register_hardware(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARD_COM_API_H__ */
