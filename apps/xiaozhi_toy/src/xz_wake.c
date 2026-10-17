/**
 * @file xz_wake.c
 * @brief Offline wake word on the T5 KWS engine.
 *
 * The platform links libtutuClear (model "nihaotuya_xiaozhitongxue_heytuya_hituya"), driven by
 * tkl_kws.c: a 2 s ring buffer and a "kws_task" thread that runs the model on 20 ms frames.
 * TuyaOpen's ai_components feed it from their own speex AEC + RNN VAD hook; this app already
 * gets AEC'd mic frames from the T5 audio driver, so it feeds them directly with the VAD flag
 * forced on (every frame is examined, about 100 ms at a time).
 *
 * The model has no "你好小智"; the closest word it knows is "小智同学".
 */

#include "tal_api.h"
#include "tkl_kws.h"

#include "xz_config.h"
#include "xz_wake.h"

/* tkl_kws.c, not in the public header (normally called by tkl_vad.c) */
extern OPERATE_RET tkl_kws_feed_with_vad(uint8_t *data, uint16_t datalen, uint8_t vadflag);

static XZ_WAKE_CB sg_cb = NULL;
static BOOL_T sg_init = FALSE;
static volatile BOOL_T sg_on = FALSE;

static void __kws_cb(TKL_KWS_WAKEUP_WORD_E word)
{
    if (!sg_on) {
        return;
    }
    if (word != TKL_KWS_WAKEUP_XIAOZHI_TONGXUE) {
        PR_DEBUG("kws: ignoring wake word %d", word);
        return;
    }
    if (sg_cb) {
        sg_cb(XZ_WAKE_WORD_TEXT);
    }
}

OPERATE_RET xz_wake_init(XZ_WAKE_CB cb)
{
    OPERATE_RET rt = OPRT_OK;

    sg_cb = cb;
    TUYA_CALL_ERR_RETURN(tkl_kws_init());
    TUYA_CALL_ERR_RETURN(tkl_kws_reg_wakeup_cb(__kws_cb));
    sg_init = TRUE;
    PR_NOTICE("wake word \"%s\" ready", XZ_WAKE_WORD_TEXT);
    return OPRT_OK;
}

void xz_wake_enable(BOOL_T on)
{
    if (!sg_init || on == sg_on) {
        return;
    }
    if (on) {
        tkl_kws_enable(); /* also drops what was buffered */
    } else {
        tkl_kws_disable();
    }
    sg_on = on;
    PR_DEBUG("wake word %s", on ? "on" : "off");
}

void xz_wake_feed(const uint8_t *pcm, uint32_t len)
{
    if (!sg_on) {
        return;
    }
    tkl_kws_feed_with_vad((uint8_t *)pcm, (uint16_t)len, 1);
}
