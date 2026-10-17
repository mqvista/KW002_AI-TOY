/**
 * @file kw002_t5ai.c
 * @brief KW002 AI toy (Tuya T5AI module) board support.
 *
 * Audio, buttons, LED and battery/charger registration. All values mirror the
 * original firmware, see docs/firmware_io_analysis.md.
 */

#include "tuya_cloud_types.h"

#include "tal_api.h"

#include "board_com_api.h"

#include "tdd_audio.h"
#include "tdd_led_gpio.h"
#include "tdd_button_gpio.h"
#if defined(POWER_NAME)
#include "tdd_power_soc.h"
#endif

/***********************************************************
***********************variable define**********************
***********************************************************/
#if defined(POWER_NAME)
static const TDD_POWER_ADC_BATTERY_T s_battery = {
    .adc_num       = TUYA_ADC_NUM_0,
    .adc_ch        = BOARD_BAT_ADC_CH,
    .divider_ratio = BOARD_BAT_DIVIDER,
    .samples       = 16, /* no board cal yet -> SoC default T5 SARADC model */
};

static const TDD_POWER_GPIO_CHARGER_T s_charger = {
    .chrg_pin     = BOARD_CHRG_PIN,
    .chrg_active  = TUYA_GPIO_LEVEL_LOW,
    .stdby_pin    = TDD_POWER_PIN_NONE, /* no STDBY line -> never reports FULL */
    .stdby_active = TUYA_GPIO_LEVEL_HIGH,
};

static const TDD_POWER_GPIO_WAKESRC_T s_wakesrc[] = {
    {BOARD_BUTTON_TALK_PIN, 0},
};

/* Voltage -> capacity table copied from the original firmware (ascending mV). */
static const TDL_POWER_OCV_PT_T s_ocv[] = {
    {3000, 1},  {3230, 5},  {3317, 10}, {3545, 20}, {3720, 30}, {3781, 40},
    {3831, 50}, {3839, 60}, {3891, 70}, {3930, 80}, {3970, 90}, {3988, 100},
};
#endif

/***********************************************************
***********************function define**********************
***********************************************************/
static OPERATE_RET __board_register_audio(void)
{
    OPERATE_RET rt = OPRT_OK;

#if defined(AUDIO_CODEC_NAME)
    TDD_AUDIO_T5AI_T cfg = {0};
    memset(&cfg, 0, sizeof(TDD_AUDIO_T5AI_T));

    /* Same as the original firmware's tkl_ai_init config. */
    cfg.aec_enable = 1;

    cfg.ai_chn      = TKL_AI_0;
    cfg.sample_rate = TKL_AUDIO_SAMPLE_16K;
    cfg.data_bits   = TKL_AUDIO_DATABITS_16;
    cfg.channel     = TKL_AUDIO_CHANNEL_MONO;

    cfg.spk_sample_rate  = TKL_AUDIO_SAMPLE_16K;
    cfg.spk_pin          = BOARD_SPEAKER_EN_PIN;
    cfg.spk_pin_polarity = TUYA_GPIO_LEVEL_LOW; /* polarity 0 -> PA driven high while playing */

    TUYA_CALL_ERR_RETURN(tdd_audio_register(AUDIO_CODEC_NAME, cfg));
#endif
    return rt;
}

static OPERATE_RET __board_register_button(void)
{
    OPERATE_RET rt = OPRT_OK;
    BUTTON_GPIO_CFG_T button_hw_cfg;

    /* Scan mode with an explicit internal pull-up: the original firmware sets both
     * keys up as pulled-up inputs, the board may have no external pull-up. */
#if defined(BUTTON_NAME)
    memset(&button_hw_cfg, 0, sizeof(BUTTON_GPIO_CFG_T));
    button_hw_cfg.pin                = BOARD_BUTTON_TALK_PIN;
    button_hw_cfg.level              = BOARD_BUTTON_ACTIVE_LV;
    button_hw_cfg.mode               = BUTTON_TIMER_SCAN_MODE;
    button_hw_cfg.pin_type.gpio_pull = TUYA_GPIO_PULLUP;

    TUYA_CALL_ERR_RETURN(tdd_gpio_button_register(BUTTON_NAME, &button_hw_cfg));
#endif

#if defined(BUTTON_NAME_2)
    memset(&button_hw_cfg, 0, sizeof(BUTTON_GPIO_CFG_T));
    button_hw_cfg.pin                = BOARD_BUTTON_RESET_PIN;
    button_hw_cfg.level              = BOARD_BUTTON_ACTIVE_LV;
    button_hw_cfg.mode               = BUTTON_TIMER_SCAN_MODE;
    button_hw_cfg.pin_type.gpio_pull = TUYA_GPIO_PULLUP;

    TUYA_CALL_ERR_RETURN(tdd_gpio_button_register(BUTTON_NAME_2, &button_hw_cfg));
#endif

    return rt;
}

static OPERATE_RET __board_register_led(void)
{
    OPERATE_RET rt = OPRT_OK;

#if defined(LED_NAME)
    TDD_LED_GPIO_CFG_T led_gpio;

    led_gpio.pin   = BOARD_LED_PIN;
    led_gpio.level = BOARD_LED_ACTIVE_LV;
    led_gpio.mode  = TUYA_GPIO_PUSH_PULL;

    TUYA_CALL_ERR_RETURN(tdd_led_gpio_register(LED_NAME, &led_gpio));
#endif

    return rt;
}

static OPERATE_RET __board_register_power(void)
{
    OPERATE_RET rt = OPRT_OK;

#if defined(POWER_NAME)
    TDD_POWER_SOC_CFG_T soc_cfg = {
        .battery     = &s_battery,
        .charger     = &s_charger,
        .wakesrc     = s_wakesrc,
        .wakesrc_cnt = sizeof(s_wakesrc) / sizeof(s_wakesrc[0]),
        .info =
            {
                .battery =
                    {
                        .v_full_mv     = 4200,
                        .v_empty_mv    = 3000,
                        .v_low_mv      = 3545, /* 20%: original fw plays the low battery alert here */
                        .v_critical_mv = 3230, /* 5% */
                        .curve         = s_ocv,
                        .curve_cnt     = sizeof(s_ocv) / sizeof(s_ocv[0]),
                    },
            },
    };

    TUYA_CALL_ERR_RETURN(tdd_power_soc_register(POWER_NAME, &soc_cfg));
#endif

    return rt;
}

/**
 * @brief Registers all the hardware peripherals (audio, buttons, LED, power) on the board.
 *
 * @return Returns OPERATE_RET_OK on success, or an appropriate error code on failure.
 */
OPERATE_RET board_register_hardware(void)
{
    OPERATE_RET rt = OPRT_OK;

    TUYA_CALL_ERR_LOG(__board_register_audio());

    TUYA_CALL_ERR_LOG(__board_register_button());

    TUYA_CALL_ERR_LOG(__board_register_led());

    TUYA_CALL_ERR_LOG(__board_register_power());

    return rt;
}
