/** Automatic standby: 5 minutes idle on battery, P33 wake after deep sleep. */
#include "tal_api.h"
#include "tkl_sleep.h"
#include "tkl_wakeup.h"
#include "tdl_power_manage.h"
#include "board_com_api.h"
#include "xz_audio.h"
#include "xz_config.h"
#include "xz_power.h"

static TDL_POWER_HANDLE sg_power            = NULL;
static uint32_t         sg_last_activity    = 0;
static uint32_t         sg_generation       = 0;
static uint32_t         sg_sleep_generation = 0;
static uint32_t         sg_retry_since      = 0;
static BOOL_T           sg_retry_wait       = FALSE;

void xz_power_activity(void)
{
    uint32_t mask    = tal_system_enter_critical();
    sg_last_activity = (uint32_t)tal_system_get_millisecond();
    sg_generation++;
    tal_system_exit_critical(mask);
}

OPERATE_RET xz_power_init(void)
{
    sg_power = tdl_power_find(POWER_NAME);
    xz_power_activity();
    if (!sg_power) {
        PR_ERR("automatic sleep disabled: power device not found");
        return OPRT_NOT_FOUND;
    }
    PR_NOTICE("automatic sleep: %u s idle, P33 wake (no voice wake in deep sleep)", XZ_IDLE_SLEEP_MS / 1000U);
    return OPRT_OK;
}

static BOOL_T __inputs_ready(void)
{
    TDL_CHG_STATE_E charger;
    /* Read CHRG directly as well as through the board power driver. Never sleep
     * on a charger read failure or while either key is held down. */
    return sg_power && board_sleep_inputs_ready() && OPRT_OK == tdl_power_charger_get_state(sg_power, &charger) &&
           charger == TDL_CHG_DISCHARGE;
}

BOOL_T xz_power_should_sleep(BOOL_T idle)
{
    if (XZ_IDLE_SLEEP_MS == 0 || !sg_power) {
        return FALSE;
    }
    if (!idle || xz_audio_is_playing() || !__inputs_ready()) {
        xz_power_activity();
        return FALSE;
    }
    uint32_t now = (uint32_t)tal_system_get_millisecond();
    if (sg_retry_wait) {
        if ((uint32_t)(now - sg_retry_since) < XZ_SLEEP_RETRY_MS) {
            return FALSE;
        }
        sg_retry_wait = FALSE;
    }
    uint32_t mask = tal_system_enter_critical();
    now           = (uint32_t)tal_system_get_millisecond();
    BOOL_T due    = (uint32_t)(now - sg_last_activity) >= XZ_IDLE_SLEEP_MS;
    if (due) {
        sg_sleep_generation = sg_generation;
    }
    tal_system_exit_critical(mask);
    return due;
}

OPERATE_RET xz_power_enter_sleep(void)
{
    OPERATE_RET rt = OPRT_OK;
    if (!__inputs_ready() || xz_audio_is_playing()) {
        return OPRT_RESOURCE_NOT_READY;
    }
    /* Preflight the same GPIO as the board backend, which does not propagate
     * tkl_wakeup_source_set errors. A successful preflight allocates its slot. */
    TUYA_WAKEUP_SOURCE_BASE_CFG_T wake   = {0};
    wake.source                          = TUYA_WAKEUP_SOURCE_GPIO;
    wake.wakeup_para.gpio_param.gpio_num = BOARD_BUTTON_TALK_PIN;
    wake.wakeup_para.gpio_param.level    = TUYA_GPIO_WAKEUP_LOW;
    TUYA_CALL_ERR_RETURN(tkl_wakeup_source_set(&wake));
    /* T5 saves parameters, resets, and executes this callback before sleeping.
     * It must not use queues, timers, PWM or initialized application state. */
    TUYA_CALL_ERR_RETURN(tkl_sleep_register_ds_user_cb(board_prepare_sleep_pins));
    uint32_t mask      = tal_system_enter_critical();
    BOOL_T   unchanged = sg_sleep_generation == sg_generation;
    tal_system_exit_critical(mask);
    if (!unchanged || !__inputs_ready()) {
        return OPRT_RESOURCE_NOT_READY;
    }
    PR_NOTICE("entering deep sleep; press P33 to wake");
    rt = tdl_power_enter_deepsleep(sg_power, 0);
    /* Success does not return. Never report a returned call as successful sleep. */
    return rt == OPRT_OK ? OPRT_COM_ERROR : rt;
}

void xz_power_defer(void)
{
    sg_retry_since = (uint32_t)tal_system_get_millisecond();
    sg_retry_wait  = TRUE;
}
