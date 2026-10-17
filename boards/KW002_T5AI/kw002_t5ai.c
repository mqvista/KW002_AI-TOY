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
#if defined(AUDIO_CODEC_NAME)
/* T5AI platform (tkl_audio.c): with the vendor VAD enabled inside the AEC pipeline,
 * the driver stops delivering mic frames after a period of silence once speech has
 * been seen, and clips the start of the next utterance. Must be called before
 * tkl_ai_init (i.e. before tdl_audio_open). */
extern void tkl_ai_disable_vendor_vad(void);
#endif

#if defined(POWER_NAME)
#include "tkl_adc.h"
#include "tdd_power_soc.h"

/* Beken SARADC calibration (efuse based, platform driver): raw code -> mV */
extern uint16_t bk_adc_data_calculate(uint16_t adc_val, uint8_t adc_chan);
#endif

/***********************************************************
************************macro define************************
***********************************************************/
#define BAT_CAL_RAW_LO     3000
#define BAT_CAL_RAW_HI     9000
#define BAT_ADC_SETTLE_MS  300

/***********************************************************
***********************variable define**********************
***********************************************************/
#if defined(POWER_NAME)
/* cal_low / cal_span are filled at boot from the chip calibration, see __board_battery_cal() */
static TDD_POWER_ADC_BATTERY_T s_battery = {
    .adc_num       = TUYA_ADC_NUM_0,
    .adc_ch        = BOARD_BAT_ADC_CH,
    .divider_ratio = BOARD_BAT_DIVIDER,
    .samples       = 16,
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

    /* continuous mic stream: wake word / VAD are done by the application */
    tkl_ai_disable_vendor_vad();

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

#if defined(POWER_NAME)
/**
 * @brief Derive the tdd_power_soc linear model from this chip's SARADC calibration.
 *
 * The generic T5 model in tdd_power_soc (1.0 V = 2469, 2429 counts/V) is about 3x
 * off for the codes tkl_adc returns here (~7500 counts/V), so sample the efuse
 * based curve at two codes and convert it to cal_low (code at 1.0 V) / cal_span.
 * Must run after a tkl_adc conversion on this channel (see __board_register_power).
 */
static void __board_battery_cal(void)
{
    uint16_t mv_lo = bk_adc_data_calculate(BAT_CAL_RAW_LO, BOARD_BAT_ADC_CH);
    uint16_t mv_hi = bk_adc_data_calculate(BAT_CAL_RAW_HI, BOARD_BAT_ADC_CH);

    if (mv_hi <= mv_lo) {
        PR_ERR("battery adc cal invalid (%u/%u mV), using SoC default model", mv_lo, mv_hi);
        return;
    }

    float counts_per_mv = (float)(BAT_CAL_RAW_HI - BAT_CAL_RAW_LO) / (float)(mv_hi - mv_lo);
    s_battery.cal_span  = (uint16_t)(counts_per_mv * 1000.0f + 0.5f);
    s_battery.cal_low   = (uint16_t)(BAT_CAL_RAW_LO + (1000.0f - mv_lo) * counts_per_mv + 0.5f);
    PR_DEBUG("battery adc cal: low %u, span %u", s_battery.cal_low, s_battery.cal_span);
}

/**
 * @brief First conversion after boot switches P28 to analog and the divider node
 *        ramps up for a while; read once and wait so the driver's filter is not
 *        seeded with a low value.
 */
static void __board_battery_settle(void)
{
    int32_t raw = 0;

    tkl_adc_read_single_channel(s_battery.adc_num, s_battery.adc_ch, &raw);
    tal_system_sleep(BAT_ADC_SETTLE_MS);
    tkl_adc_read_single_channel(s_battery.adc_num, s_battery.adc_ch, &raw);
}
#endif

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

    /* Order matters: the calibration curve depends on the SARADC config that tkl_adc
     * applies on a read, so sample it only after the driver initialized the channel
     * and did a conversion. The driver reads cal_low/cal_span through the pointer. */
    __board_battery_settle();
    __board_battery_cal();
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
