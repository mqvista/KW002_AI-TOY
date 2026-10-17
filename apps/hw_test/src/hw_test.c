/**
 * @file hw_test.c
 * @brief KW002 hardware self test.
 *
 * Verifies the pin map recovered from the original firmware:
 *   - LED (P8): 2 s on / 2 s off at boot, then slow blink   -> check the polarity
 *   - talk key (P33): hold to record (max 5 s), release to play it back -> mic, PA (P19), speaker
 *   - reset key (P12): click plays a 1 kHz tone, long press (3 s) is logged
 *   - battery (P28/ADC4) and charger (P21): logged every 5 s and on charge state change
 */

#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "tkl_memory.h"
#include "tuya_ringbuf.h"

#include "board_com_api.h"
#include "tdl_audio_manage.h"
#include "tdl_button_manage.h"
#include "tdl_led_manage.h"
#if defined(POWER_NAME)
#include "tdl_power_manage.h"
#include "tkl_adc.h"

/* Beken SARADC calibration (platform driver), returns mV for a raw code */
extern uint16_t bk_adc_data_calculate(uint16_t adc_val, uint8_t adc_chan);
#endif

/***********************************************************
************************macro define************************
***********************************************************/
#define RECORD_MAX_MS     (5 * 1000)
#define TONE_MS           1000
#define STATUS_PERIOD_MS  5000
#define PLAY_VOLUME       70

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef enum {
    REC_IDLE = 0,
    REC_START,
    REC_RECORDING,
    REC_END,
    REC_TONE,
} REC_STATUS_E;

/***********************************************************
***********************variable define**********************
***********************************************************/
static volatile REC_STATUS_E sg_rec_status = REC_IDLE;
static TDL_AUDIO_HANDLE_T sg_audio_hdl = NULL;
static TDL_AUDIO_INFO_T sg_audio_info = {0};
static TUYA_RINGBUFF_T sg_rec_rb = NULL;
static TDL_LED_HANDLE_T sg_led_hdl = NULL;
#if defined(POWER_NAME)
static TDL_POWER_HANDLE sg_pwr_hdl = NULL;
#endif

/* One 1 kHz period at 16 kHz, amplitude ~30 % of full scale. */
static const int16_t sg_tone_1k[16] = {
    0, 3762, 6951, 9082, 9830, 9082, 6951, 3762, 0, -3762, -6951, -9082, -9830, -9082, -6951, -3762,
};

/***********************************************************
***********************function define**********************
***********************************************************/
static void __mic_frame_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status, uint8_t *data, uint32_t len)
{
    if (REC_RECORDING != sg_rec_status || NULL == sg_rec_rb) {
        return;
    }

    if (tuya_ring_buff_free_size_get(sg_rec_rb) < len) {
        PR_NOTICE("record buffer full (%d ms), stop", RECORD_MAX_MS);
        sg_rec_status = REC_END;
        return;
    }
    tuya_ring_buff_write(sg_rec_rb, data, len);
}

static void __talk_button_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *argc)
{
    switch (event) {
    case TDL_BUTTON_PRESS_DOWN:
        PR_NOTICE("[KEY] %s (P33) down", name);
        if (REC_IDLE == sg_rec_status) {
            sg_rec_status = REC_START;
        }
        break;
    case TDL_BUTTON_PRESS_UP:
        PR_NOTICE("[KEY] %s (P33) up", name);
        if (REC_RECORDING == sg_rec_status) {
            sg_rec_status = REC_END;
        }
        break;
    case TDL_BUTTON_LONG_PRESS_START:
        PR_NOTICE("[KEY] %s (P33) long press", name);
        break;
    default:
        break;
    }
}

static void __reset_button_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *argc)
{
    switch (event) {
    case TDL_BUTTON_PRESS_DOWN:
        PR_NOTICE("[KEY] %s (P12) down", name);
        break;
    case TDL_BUTTON_PRESS_UP:
        PR_NOTICE("[KEY] %s (P12) up", name);
        break;
    case TDL_BUTTON_PRESS_SINGLE_CLICK:
        PR_NOTICE("[KEY] %s (P12) click -> 1 kHz tone", name);
        if (REC_IDLE == sg_rec_status) {
            sg_rec_status = REC_TONE;
        }
        break;
    case TDL_BUTTON_LONG_PRESS_START:
        PR_NOTICE("[KEY] %s (P12) long press 3 s (will be: re-provision Wi-Fi)", name);
        break;
    default:
        break;
    }
}

static OPERATE_RET __buttons_init(void)
{
    OPERATE_RET rt = OPRT_OK;
    TDL_BUTTON_CFG_T button_cfg = {
        .long_start_valid_time     = 3000,
        .long_keep_timer           = 1000,
        .button_debounce_time      = 50,
        .button_repeat_valid_count = 0,
        .button_repeat_valid_time  = 500,
    };
    TDL_BUTTON_HANDLE talk_hdl  = NULL;
    TDL_BUTTON_HANDLE reset_hdl = NULL;

    TUYA_CALL_ERR_RETURN(tdl_button_create(BUTTON_NAME, &button_cfg, &talk_hdl));
    tdl_button_event_register(talk_hdl, TDL_BUTTON_PRESS_DOWN, __talk_button_cb);
    tdl_button_event_register(talk_hdl, TDL_BUTTON_PRESS_UP, __talk_button_cb);
    tdl_button_event_register(talk_hdl, TDL_BUTTON_LONG_PRESS_START, __talk_button_cb);

    TUYA_CALL_ERR_RETURN(tdl_button_create(BUTTON_NAME_2, &button_cfg, &reset_hdl));
    tdl_button_event_register(reset_hdl, TDL_BUTTON_PRESS_DOWN, __reset_button_cb);
    tdl_button_event_register(reset_hdl, TDL_BUTTON_PRESS_UP, __reset_button_cb);
    tdl_button_event_register(reset_hdl, TDL_BUTTON_PRESS_SINGLE_CLICK, __reset_button_cb);
    tdl_button_event_register(reset_hdl, TDL_BUTTON_LONG_PRESS_START, __reset_button_cb);

    return rt;
}

static OPERATE_RET __audio_init(void)
{
    OPERATE_RET rt = OPRT_OK;
    uint32_t buf_len = 0;

    TUYA_CALL_ERR_RETURN(tdl_audio_find(AUDIO_CODEC_NAME, &sg_audio_hdl));
    TUYA_CALL_ERR_RETURN(tdl_audio_open(sg_audio_hdl, __mic_frame_cb));
    TUYA_CALL_ERR_RETURN(tdl_audio_get_info(sg_audio_hdl, &sg_audio_info));
    if (0 == sg_audio_info.frame_size || 0 == sg_audio_info.sample_tm_ms) {
        PR_ERR("get audio info err");
        return OPRT_INVALID_PARM;
    }
    PR_NOTICE("[AUDIO] %d Hz, %d bit, %d ch, frame %d ms / %d bytes", sg_audio_info.sample_rate,
              sg_audio_info.sample_bits, sg_audio_info.sample_ch_num, sg_audio_info.sample_tm_ms,
              sg_audio_info.frame_size);

    buf_len = (RECORD_MAX_MS / sg_audio_info.sample_tm_ms) * sg_audio_info.frame_size;
    TUYA_CALL_ERR_RETURN(tuya_ring_buff_create(buf_len, OVERFLOW_PSRAM_STOP_TYPE, &sg_rec_rb));
    TUYA_CALL_ERR_RETURN(tdl_audio_volume_set(sg_audio_hdl, PLAY_VOLUME));

    return rt;
}

static void __play_record(void)
{
    uint32_t total = tuya_ring_buff_used_size_get(sg_rec_rb);
    uint8_t *frame = tkl_system_psram_malloc(sg_audio_info.frame_size);

    if (NULL == frame) {
        PR_ERR("psram malloc failed");
        tuya_ring_buff_reset(sg_rec_rb);
        return;
    }

    PR_NOTICE("[AUDIO] play back %u bytes (%u ms)", total,
              total / sg_audio_info.frame_size * sg_audio_info.sample_tm_ms);
    while (tuya_ring_buff_used_size_get(sg_rec_rb) > 0) {
        uint32_t len = tuya_ring_buff_read(sg_rec_rb, frame, sg_audio_info.frame_size);
        tdl_audio_play(sg_audio_hdl, frame, len);
    }

    tkl_system_psram_free(frame);
}

static void __play_tone(void)
{
    uint32_t frame_samples = sg_audio_info.frame_size / sizeof(int16_t);
    uint32_t frames = TONE_MS / sg_audio_info.sample_tm_ms;
    int16_t *frame = tkl_system_psram_malloc(sg_audio_info.frame_size);
    uint32_t phase = 0;

    if (NULL == frame) {
        PR_ERR("psram malloc failed");
        return;
    }

    for (uint32_t f = 0; f < frames; f++) {
        for (uint32_t i = 0; i < frame_samples; i++) {
            frame[i] = sg_tone_1k[phase++ & 0x0F];
        }
        tdl_audio_play(sg_audio_hdl, (uint8_t *)frame, sg_audio_info.frame_size);
    }

    tkl_system_psram_free(frame);
}

static void __led_init(void)
{
    PR_NOTICE("[LED] find %s", LED_NAME);
    sg_led_hdl = tdl_led_find_dev(LED_NAME);
    PR_NOTICE("[LED] handle %p, open", sg_led_hdl);
    if (NULL == sg_led_hdl || OPRT_OK != tdl_led_open(sg_led_hdl)) {
        PR_ERR("led open failed");
        sg_led_hdl = NULL;
        return;
    }

    PR_NOTICE("[LED] P8 should be ON for 2 s");
    tdl_led_set_status(sg_led_hdl, TDL_LED_ON);
    tal_system_sleep(2000);
    PR_NOTICE("[LED] P8 should be OFF for 2 s");
    tdl_led_set_status(sg_led_hdl, TDL_LED_OFF);
    tal_system_sleep(2000);
    PR_NOTICE("[LED] P8 slow blink");
    tdl_led_flash(sg_led_hdl, 1000);
}

#if defined(POWER_NAME)
static const char *__chg_str(TDL_CHG_STATE_E st)
{
    switch (st) {
    case TDL_CHG_CHARGING:
        return "charging";
    case TDL_CHG_FULL:
        return "full";
    default:
        return "discharging";
    }
}

static void __charge_evt_cb(TDL_CHG_STATE_E state, void *arg)
{
    PR_NOTICE("[POWER] charger (P21) -> %s", __chg_str(state));
}

static void __power_init(void)
{
    sg_pwr_hdl = tdl_power_find(POWER_NAME);
    if (NULL == sg_pwr_hdl) {
        PR_ERR("power device not found");
        return;
    }
    tdl_power_charger_on_event(sg_pwr_hdl, __charge_evt_cb, NULL);
}

static void __power_report(void)
{
    uint32_t mv = 0;
    uint8_t pct = 0;
    TDL_CHG_STATE_E st = TDL_CHG_DISCHARGE;

    if (NULL == sg_pwr_hdl) {
        return;
    }

    /* raw SARADC codes on P28, to calibrate the board divider / model */
    int32_t raw[16] = {0};
    int32_t rmin = 0x7fffffff, rmax = 0, rsum = 0;
    for (int i = 0; i < 16; i++) {
        tkl_adc_read_single_channel(TUYA_ADC_NUM_0, BOARD_BAT_ADC_CH, &raw[i]);
        rmin = (raw[i] < rmin) ? raw[i] : rmin;
        rmax = (raw[i] > rmax) ? raw[i] : rmax;
        rsum += raw[i];
    }
    PR_NOTICE("[ADC] ch%d raw min %d max %d avg(trim) %d | %d %d %d %d %d %d %d %d", BOARD_BAT_ADC_CH, rmin, rmax,
              (rsum - rmin - rmax) / 14, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);

    /* chip calibration curve (efuse based), used by tkl_adc_read_voltage and the original firmware */
    PR_NOTICE("[ADC] cal mV @raw 3000:%u 5000:%u 7000:%u 9000:%u | avg %d -> %u mV x%d = %u mV",
              bk_adc_data_calculate(3000, BOARD_BAT_ADC_CH), bk_adc_data_calculate(5000, BOARD_BAT_ADC_CH),
              bk_adc_data_calculate(7000, BOARD_BAT_ADC_CH), bk_adc_data_calculate(9000, BOARD_BAT_ADC_CH),
              (rsum - rmin - rmax) / 14, bk_adc_data_calculate((rsum - rmin - rmax) / 14, BOARD_BAT_ADC_CH),
              (int)BOARD_BAT_DIVIDER,
              (uint32_t)(bk_adc_data_calculate((rsum - rmin - rmax) / 14, BOARD_BAT_ADC_CH) * BOARD_BAT_DIVIDER));

    tdl_power_battery_get_voltage(sg_pwr_hdl, &mv);
    tdl_power_battery_get_percent(sg_pwr_hdl, &pct);
    tdl_power_charger_get_state(sg_pwr_hdl, &st);
    PR_NOTICE("[POWER] battery %u mV (P28 x%d), %u pct, %s", mv, (int)BOARD_BAT_DIVIDER, pct, __chg_str(st));
}
#endif

static void user_main(void)
{
    OPERATE_RET rt = OPRT_OK;
    uint32_t idle_ms = 0;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("==== KW002 hardware self test ====");
    PR_NOTICE("TuyaOpen %s (%s), board %s", OPEN_VERSION, OPEN_COMMIT, PLATFORM_BOARD);

    /* LED flash/blink and button debounce run on TAL software timers / work queue */
    tal_sw_timer_init();
    tal_workq_init();

    TUYA_CALL_ERR_LOG(board_register_hardware());
    TUYA_CALL_ERR_LOG(__audio_init());
    TUYA_CALL_ERR_LOG(__buttons_init());
#if defined(POWER_NAME)
    __power_init();
    __power_report();
#endif
    __led_init();

    PR_NOTICE("Ready: hold P33 to record / release to play, click P12 for a tone");

    while (1) {
        switch (sg_rec_status) {
        case REC_START:
            tuya_ring_buff_reset(sg_rec_rb);
            PR_NOTICE("[AUDIO] recording...");
            sg_rec_status = REC_RECORDING;
            break;
        case REC_END:
            __play_record();
            sg_rec_status = REC_IDLE;
            break;
        case REC_TONE:
            __play_tone();
            sg_rec_status = REC_IDLE;
            break;
        default:
            break;
        }

        tal_system_sleep(10);
        idle_ms += 10;
        if (idle_ms >= STATUS_PERIOD_MS) {
            idle_ms = 0;
#if defined(POWER_NAME)
            __power_report();
#endif
        }
    }
}

/* Tuya thread handle */
static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = 1024 * 4;
    thrd_param.priority = THREAD_PRIO_1;
    thrd_param.thrdname = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
