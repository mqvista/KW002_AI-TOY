/**
 * @file xz_battery.c
 * @brief Battery level and charger state.
 *
 * The board registers tdd_power_soc (P28 = Vbat/4 on ADC4, calibrated at boot; P21 low =
 * charging) with the original firmware's 12 point OCV table. One tdl_power reading
 * averages 16 ADC samples; on top of that:
 *   - samples are taken every 10 s, only after the speaker was quiet for 2 s: at 100 %
 *     volume the amplifier pulls the cell down far enough to fake a low battery;
 *   - the last 6 samples (one minute) are averaged before the table lookup.
 * While charging the terminal voltage reads high, the level is still reported (it rises
 * as the cell charges) but the caller should not take low battery decisions on it.
 */

#include "tal_api.h"
#include "tdl_power_manage.h"

#include "xz_battery.h"

#define SAMPLE_PERIOD_MS 10000
#define QUIET_MS         2000
#define AVG_N            6

static TDL_POWER_HANDLE sg_pwr = NULL;
static TDL_POWER_BATTERY_INFO_T sg_info;
static XZ_BATTERY_CHG_CB sg_chg_cb = NULL;
static volatile BOOL_T sg_charging = FALSE;

static uint16_t sg_mv[AVG_N];
static uint8_t sg_cnt = 0;
static uint8_t sg_idx = 0;
static SYS_TIME_T sg_next_sample = 0;
static SYS_TIME_T sg_busy_ms = 0; /* last time the speaker was seen busy */

/* Same interpolation as tdl_power (ascending curve, linear between points). */
static uint8_t __level_from_mv(uint32_t mv)
{
    const TDL_POWER_OCV_PT_T *c = sg_info.curve;
    uint8_t n = sg_info.curve_cnt;

    if (NULL == c || n < 2) {
        if (mv <= sg_info.v_empty_mv) {
            return 0;
        }
        if (mv >= sg_info.v_full_mv) {
            return 100;
        }
        return (uint8_t)((mv - sg_info.v_empty_mv) * 100 / (sg_info.v_full_mv - sg_info.v_empty_mv));
    }
    if (mv <= c[0].mv) {
        return mv < sg_info.v_empty_mv ? 0 : c[0].pct;
    }
    if (mv >= c[n - 1].mv) {
        return c[n - 1].pct;
    }
    for (uint8_t i = 1; i < n; i++) {
        if (mv <= c[i].mv) {
            return (uint8_t)(c[i - 1].pct + (mv - c[i - 1].mv) * (c[i].pct - c[i - 1].pct) / (c[i].mv - c[i - 1].mv));
        }
    }
    return 100;
}

static void __chg_cb(TDL_CHG_STATE_E state, void *arg)
{
    (void)arg;
    sg_charging = (TDL_CHG_CHARGING == state);
    if (sg_chg_cb) {
        sg_chg_cb(sg_charging);
    }
}

OPERATE_RET xz_battery_init(XZ_BATTERY_CHG_CB cb)
{
    TDL_POWER_INFO_T info;
    TDL_CHG_STATE_E st = TDL_CHG_DISCHARGE;

    sg_pwr = tdl_power_find(POWER_NAME);
    if (NULL == sg_pwr) {
        PR_ERR("power device %s not found", POWER_NAME);
        return OPRT_NOT_FOUND;
    }
    if (OPRT_OK == tdl_power_get_info(sg_pwr, &info)) {
        sg_info = info.battery;
    }
    if (OPRT_OK == tdl_power_charger_get_state(sg_pwr, &st)) {
        sg_charging = (TDL_CHG_CHARGING == st);
    }
    sg_chg_cb = cb;
    tdl_power_charger_on_event(sg_pwr, __chg_cb, NULL);
    return OPRT_OK;
}

void xz_battery_poll(BOOL_T speaker_busy)
{
    SYS_TIME_T now = tal_system_get_millisecond();
    uint32_t mv = 0;

    if (NULL == sg_pwr) {
        return;
    }
    if (speaker_busy) {
        sg_busy_ms = now;
        return;
    }
    if (now < sg_next_sample || now - sg_busy_ms < QUIET_MS) {
        return;
    }
    sg_next_sample = now + SAMPLE_PERIOD_MS;
    if (OPRT_OK != tdl_power_battery_get_voltage(sg_pwr, &mv) || mv == 0) {
        return;
    }
    if (0 == sg_cnt) {
        for (int i = 0; i < AVG_N; i++) {
            sg_mv[i] = (uint16_t)mv; /* first sample fills the window */
        }
        sg_cnt = AVG_N;
        PR_NOTICE("battery %u mV, %u pct, %s", mv, __level_from_mv(mv), sg_charging ? "charging" : "discharging");
    }
    sg_mv[sg_idx] = (uint16_t)mv;
    sg_idx = (sg_idx + 1) % AVG_N;
}

BOOL_T xz_battery_get(XZ_BATTERY_T *out)
{
    uint32_t sum = 0;

    out->charging = sg_charging;
    if (0 == sg_cnt) {
        out->mv = 0;
        out->level = 0;
        return FALSE;
    }
    for (int i = 0; i < AVG_N; i++) {
        sum += sg_mv[i];
    }
    out->mv = (uint16_t)(sum / AVG_N);
    out->level = __level_from_mv(out->mv);
    out->charging = sg_charging;
    return TRUE;
}
