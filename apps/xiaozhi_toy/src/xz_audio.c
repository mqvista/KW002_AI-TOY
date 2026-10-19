/**
 * @file xz_audio.c
 * @brief Audio service: mic -> Opus encoder -> sender, Opus packets / prompts -> decoder -> speaker.
 *
 * Threads:
 *   xz_enc  takes 20 ms mic frames from a queue, encodes 60 ms Opus packets, calls the send cb
 *   xz_play takes Opus packets from a queue, decodes them to 16 kHz PCM and writes the speaker.
 *           tdl_audio_play() accepts about 1 s ahead and then blocks, which paces playback.
 */

#include <string.h>

#include "tal_api.h"
#include "tkl_memory.h"
#include "tdl_audio_manage.h"
#include "opus.h"

#include "board_com_api.h"
#include "xz_audio.h"
#include "xz_config.h"

/***********************************************************
************************macro define************************
***********************************************************/
#define MIC_FRAME_BYTES     640                       /* 20 ms @ 16 kHz mono 16 bit */
#define MIC_QUEUE_DEPTH     25                        /* 0.5 s */
#define ENC_FRAME_BYTES     (XZ_OPUS_FRAME_SAMPLES * 2)
#define ENC_MAX_PACKET      512
#define ENC_COMPLEXITY      3

#define PLAY_QUEUE_DEPTH    256                       /* packets (60 ms each from the server) */
#define DEC_MAX_SAMPLES     (XZ_SAMPLE_RATE / 1000 * 120) /* longest Opus frame */

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    uint16_t len;      /* 0: decoder reset marker */
    uint8_t data[];
} PLAY_PKT_T;

/***********************************************************
***********************variable define**********************
***********************************************************/
static TDL_AUDIO_HANDLE_T sg_audio = NULL;

static QUEUE_HANDLE sg_mic_queue = NULL;
static volatile BOOL_T sg_capture_on = FALSE;
static XZ_AUDIO_SEND_CB sg_send_cb = NULL;
static XZ_AUDIO_TAP_CB sg_mic_tap = NULL;
static uint8_t sg_volume = XZ_SPEAKER_VOLUME;
static OpusEncoder *sg_encoder = NULL;

static QUEUE_HANDLE sg_play_queue = NULL;
static OpusDecoder *sg_decoder = NULL;
static MUTEX_HANDLE sg_play_mutex = NULL;
static volatile uint32_t sg_play_pending = 0;   /* packets queued, not yet written */
static volatile uint32_t sg_play_end_ms    = 0;   /* projected end of what was written */
static volatile BOOL_T sg_flush = FALSE;
static volatile BOOL_T   sg_pa_on          = FALSE;
static volatile BOOL_T   sg_play_end_valid = FALSE;
static volatile BOOL_T   sg_paused         = FALSE;
static volatile BOOL_T   sg_pause_ack      = FALSE;
static BOOL_T            sg_device_open    = FALSE;

static THREAD_HANDLE sg_enc_thread = NULL;
static THREAD_HANDLE sg_play_thread = NULL;

/***********************************************************
***********************function define**********************
***********************************************************/
static void __mic_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status, uint8_t *data, uint32_t len)
{
    (void)type;
    (void)status;
    if (sg_paused) {
        return;
    }
    if (sg_mic_tap) {
        sg_mic_tap(data, len);
    }
    if (!sg_capture_on) {
        return;
    }
    /* mic frames are 640 bytes; split anything else into 640 byte chunks */
    while (len >= MIC_FRAME_BYTES) {
        if (OPRT_OK != tal_queue_post(sg_mic_queue, data, 0)) {
            PR_WARN("mic queue full, frame dropped");
        }
        data += MIC_FRAME_BYTES;
        len -= MIC_FRAME_BYTES;
    }
}

static void __enc_task(void *arg)
{
    uint8_t *pcm = tkl_system_psram_malloc(ENC_FRAME_BYTES);
    uint8_t *frame = tkl_system_psram_malloc(MIC_FRAME_BYTES);
    uint8_t *packet = tkl_system_psram_malloc(ENC_MAX_PACKET);
    uint32_t fill = 0;
    BOOL_T was_on = FALSE;

    (void)arg;
    if (!pcm || !frame || !packet) {
        PR_ERR("encoder buffers alloc failed");
        return;
    }

    while (1) {
        if (OPRT_OK != tal_queue_fetch(sg_mic_queue, frame, 100)) {
            if (was_on && !sg_capture_on) {
                fill = 0;
                opus_encoder_ctl(sg_encoder, OPUS_RESET_STATE);
                was_on = FALSE;
            }
            continue;
        }
        if (!sg_capture_on) {
            continue; /* stale frame after capture stopped */
        }
        was_on = TRUE;

        memcpy(pcm + fill, frame, MIC_FRAME_BYTES);
        fill += MIC_FRAME_BYTES;
        if (fill < ENC_FRAME_BYTES) {
            continue;
        }
        fill = 0;

        opus_int32 n = opus_encode(sg_encoder, (const opus_int16 *)pcm, XZ_OPUS_FRAME_SAMPLES, packet, ENC_MAX_PACKET);
        if (n < 0) {
            PR_ERR("opus_encode error %d", (int)n);
            continue;
        }
        if (sg_send_cb && sg_capture_on) {
            sg_send_cb(packet, (uint32_t)n);
        }
    }
}

static void __pending_add(int delta)
{
    tal_mutex_lock(sg_play_mutex);
    if (delta < 0 && sg_play_pending == 0) {
        delta = 0;
    }
    sg_play_pending += delta;
    tal_mutex_unlock(sg_play_mutex);
}

static void __play_free_all(void)
{
    PLAY_PKT_T *pkt = NULL;

    while (OPRT_OK == tal_queue_fetch(sg_play_queue, &pkt, 0)) {
        tal_free(pkt);
        __pending_add(-1);
    }
}

/* The playback worker alone owns PA transitions and the audio write path. */
static OPERATE_RET __speaker_set(BOOL_T on)
{
    if (on == sg_pa_on) {
        return OPRT_OK;
    }
    OPERATE_RET rt = board_speaker_set(on);
    if (OPRT_OK == rt) {
        sg_pa_on = on;
        PR_DEBUG("amplifier %s", on ? "on" : "off");
        if (on) {
            tal_system_sleep(XZ_PA_START_MS);
        }
    } else {
        PR_ERR("amplifier switch failed: %d", rt);
    }
    return rt;
}

static BOOL_T __pcm_draining(uint32_t now)
{
    return sg_play_end_valid && (int32_t)((uint32_t)sg_play_end_ms - now) > 0;
}

static void __speaker_idle_poll(void)
{
    uint32_t now = (uint32_t)tal_system_get_millisecond();
    if (sg_volume == 0) {
        __speaker_set(FALSE);
    } else if (__pcm_draining(now)) {
        __speaker_set(TRUE); /* also handles unmuting while PCM is still buffered */
    }
    if (sg_play_end_valid && (int32_t)(now - (uint32_t)sg_play_end_ms) >= (int32_t)XZ_PA_TAIL_MS) {
        sg_play_end_valid = FALSE;
    }
    if (sg_play_pending == 0 && !sg_play_end_valid) {
        __speaker_set(FALSE);
    }
}

static void __play_step(int16_t *pcm)
{
    PLAY_PKT_T *pkt = NULL;
    if (sg_paused) {
        if (!sg_pause_ack) {
            __play_free_all();
            __speaker_set(FALSE);
            sg_play_end_valid = FALSE;
            sg_flush = FALSE;
            sg_pause_ack      = TRUE;
        }
        tal_system_sleep(20);
        return;
    }
    sg_pause_ack = FALSE;
    if (sg_flush) {
        __play_free_all();
        tdl_audio_play_stop(sg_audio);
        opus_decoder_ctl(sg_decoder, OPUS_RESET_STATE);
        sg_play_end_valid = FALSE;
        __speaker_set(FALSE);
        sg_flush = FALSE;
    }

    __speaker_idle_poll();
    if (OPRT_OK != tal_queue_fetch(sg_play_queue, &pkt, 50)) {
        __speaker_idle_poll();
        return;
    }
    if (pkt->len == 0) {
        opus_decoder_ctl(sg_decoder, OPUS_RESET_STATE);
    } else if (!sg_flush && !sg_paused) {
        int samples = opus_decode(sg_decoder, pkt->data, pkt->len, pcm, DEC_MAX_SAMPLES, 0);
        if (samples > 0) {
            OPERATE_RET rt = __speaker_set(sg_volume != 0);
            if (OPRT_OK == rt) {
                uint32_t now = (uint32_t)tal_system_get_millisecond();
                uint32_t dur = (uint32_t)samples * 1000U / XZ_SAMPLE_RATE;
                uint32_t end = __pcm_draining(now) ? (uint32_t)sg_play_end_ms : now;
                end += dur;
                rt  = tdl_audio_play(sg_audio, (uint8_t *)pcm, (uint32_t)samples * sizeof(int16_t));
                now = (uint32_t)tal_system_get_millisecond();
                if (OPRT_OK == rt) {
                    /* A blocked write may outlive our projection. Leave a full frame
                     * of margin rather than switching PA off on newly accepted PCM. */
                    sg_play_end_ms    = (int32_t)(end - now) > 0 ? end : now + dur;
                    sg_play_end_valid = TRUE;
                }
            }
            if (OPRT_OK != rt) {
                PR_ERR("speaker write failed: %d", rt);
                __play_free_all();
                sg_play_end_valid = FALSE;
                __speaker_set(FALSE);
            }
        } else {
            PR_WARN("opus_decode error %d (len %u)", samples, pkt->len);
        }
    }
    tal_free(pkt);
    __pending_add(-1);
    __speaker_idle_poll();
}

static void __play_task(void *arg)
{
    int16_t *pcm = tkl_system_psram_malloc(DEC_MAX_SAMPLES * sizeof(int16_t));
    (void)arg;
    if (!pcm) {
        PR_ERR("decoder buffer alloc failed");
        return;
    }
    while (1) {
        __play_step(pcm);
    }
}

static void __play_push(const uint8_t *data, uint16_t len)
{
    PLAY_PKT_T *pkt = tal_malloc(sizeof(PLAY_PKT_T) + len);
    if (NULL == pkt) {
        PR_ERR("play packet alloc failed");
        return;
    }
    pkt->len = len;
    if (len) {
        memcpy(pkt->data, data, len);
    }
    tal_mutex_lock(sg_play_mutex);
    /* Serialize the gate with queue insertion. A sleep request must prevent a
     * producer from adding another packet after the worker acknowledged pause. */
    if (sg_paused) {
        tal_mutex_unlock(sg_play_mutex);
        tal_free(pkt);
        return;
    }
    sg_play_pending++;
    if (OPRT_OK != tal_queue_post(sg_play_queue, &pkt, 0)) {
        sg_play_pending--;
        tal_free(pkt);
        PR_WARN("play queue full, packet dropped");
    }
    tal_mutex_unlock(sg_play_mutex);
}

/* Split an Ogg/Opus file into Opus packets and queue them (skips OpusHead/OpusTags). */
static void __play_ogg(const uint8_t *ogg, size_t size)
{
    static uint8_t packet[1024];
    uint32_t pkt_len = 0;
    uint32_t pkt_idx = 0;
    size_t pos = 0;

    __play_push(NULL, 0); /* new stream: reset the decoder */

    while (pos + 27 <= size) {
        if (memcmp(ogg + pos, "OggS", 4) != 0) {
            PR_ERR("ogg: bad page at %u", (unsigned)pos);
            return;
        }
        uint8_t nseg = ogg[pos + 26];
        const uint8_t *lacing = ogg + pos + 27;
        const uint8_t *body = lacing + nseg;
        size_t body_off = 0;

        if (pos + 27 + nseg > size) {
            return;
        }
        for (uint8_t i = 0; i < nseg; i++) {
            uint8_t seg = lacing[i];
            if (pkt_len + seg <= sizeof(packet) && body + body_off + seg <= ogg + size) {
                memcpy(packet + pkt_len, body + body_off, seg);
            }
            pkt_len += seg;
            body_off += seg;
            if (seg < 255) { /* packet complete */
                if (pkt_idx >= 2 && pkt_len > 0 && pkt_len <= sizeof(packet)) {
                    __play_push(packet, (uint16_t)pkt_len);
                }
                pkt_idx++;
                pkt_len = 0;
            }
        }
        pos += 27 + nseg + body_off;
    }
}

OPERATE_RET xz_audio_init(void)
{
    OPERATE_RET rt = OPRT_OK;
    int err = 0;

    TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&sg_play_mutex));
    TUYA_CALL_ERR_RETURN(tal_queue_create_init(&sg_mic_queue, MIC_FRAME_BYTES, MIC_QUEUE_DEPTH));
    TUYA_CALL_ERR_RETURN(tal_queue_create_init(&sg_play_queue, sizeof(PLAY_PKT_T *), PLAY_QUEUE_DEPTH));

    sg_encoder = opus_encoder_create(XZ_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (NULL == sg_encoder || OPUS_OK != err) {
        PR_ERR("opus_encoder_create failed %d", err);
        return OPRT_COM_ERROR;
    }
    opus_encoder_ctl(sg_encoder, OPUS_SET_COMPLEXITY(ENC_COMPLEXITY));

    sg_decoder = opus_decoder_create(XZ_SAMPLE_RATE, 1, &err);
    if (NULL == sg_decoder || OPUS_OK != err) {
        PR_ERR("opus_decoder_create failed %d", err);
        return OPRT_COM_ERROR;
    }

    TUYA_CALL_ERR_RETURN(tdl_audio_find(AUDIO_CODEC_NAME, &sg_audio));
    TUYA_CALL_ERR_RETURN(tdl_audio_open(sg_audio, __mic_cb));
    sg_device_open = TRUE;
    TUYA_CALL_ERR_LOG(tdl_audio_volume_set(sg_audio, sg_volume));

    THREAD_CFG_T enc_cfg = {.stackDepth = 40 * 1024, .priority = THREAD_PRIO_2, .thrdname = "xz_enc"};
    TUYA_CALL_ERR_RETURN(tal_thread_create_and_start(&sg_enc_thread, NULL, NULL, __enc_task, NULL, &enc_cfg));
    THREAD_CFG_T play_cfg = {.stackDepth = 16 * 1024, .priority = THREAD_PRIO_2, .thrdname = "xz_play"};
    TUYA_CALL_ERR_RETURN(tal_thread_create_and_start(&sg_play_thread, NULL, NULL, __play_task, NULL, &play_cfg));

    sg_play_end_ms = tal_system_get_millisecond();
    return rt;
}

void xz_audio_capture(BOOL_T on, XZ_AUDIO_SEND_CB cb)
{
    if (on) {
        sg_send_cb = cb;
    }
    sg_capture_on = on;
}

void xz_audio_set_mic_tap(XZ_AUDIO_TAP_CB cb)
{
    sg_mic_tap = cb;
}

void xz_audio_set_volume(uint8_t volume)
{
    sg_volume = volume > 100 ? 100 : volume;
    if (sg_audio && sg_device_open && !sg_paused) {
        tdl_audio_volume_set(sg_audio, sg_volume);
    }
}

uint8_t xz_audio_get_volume(void)
{
    return sg_volume;
}

void xz_audio_feed_opus(const uint8_t *data, uint32_t len)
{
    if (len == 0 || len > 0xFFFF) {
        return;
    }
    __play_push(data, (uint16_t)len);
}

BOOL_T xz_audio_is_playing(void)
{
    return sg_play_pending > 0 || sg_flush || sg_pa_on || __pcm_draining((uint32_t)tal_system_get_millisecond());
}

void xz_audio_wait_idle(uint32_t timeout_ms)
{
    SYS_TIME_T deadline = tal_system_get_millisecond() + timeout_ms;

    while (xz_audio_is_playing() && tal_system_get_millisecond() < deadline) {
        tal_system_sleep(20);
    }
}

void xz_audio_play_sound(XZ_SOUND_E snd, BOOL_T wait)
{
    if (snd >= XZ_SND_MAX) {
        return;
    }
    __play_ogg(g_xz_sounds[snd].data, g_xz_sounds[snd].len);
    if (wait) {
        xz_audio_wait_idle(15000);
    }
}

void xz_audio_play_digits(const char *digits)
{
    for (const char *d = digits; *d; d++) {
        if (*d >= '0' && *d <= '9') {
            __play_ogg(g_xz_sounds[XZ_SND_0 + (*d - '0')].data, g_xz_sounds[XZ_SND_0 + (*d - '0')].len);
        }
    }
    xz_audio_wait_idle(15000);
}

void xz_audio_stop_playback(void)
{
    sg_flush = TRUE;
    /* the play thread clears the flag after flushing */
    for (int i = 0; i < 50 && sg_flush; i++) {
        tal_system_sleep(10);
    }
}

/* Stop producers, wait for the playback worker to acknowledge, then close the
 * codec. No tdl_audio_play() can overlap with tdl_audio_close(). */
OPERATE_RET xz_audio_suspend(void)
{
    xz_audio_capture(FALSE, NULL);
    tal_mutex_lock(sg_play_mutex);
    sg_paused = TRUE;
    tal_mutex_unlock(sg_play_mutex);
    uint32_t start = (uint32_t)tal_system_get_millisecond();
    while (!sg_pause_ack) {
        if ((uint32_t)((uint32_t)tal_system_get_millisecond() - start) >= 1500U) {
            return OPRT_TIMEOUT;
        }
        tal_system_sleep(10);
    }
    if (sg_device_open) {
        OPERATE_RET rt = tdl_audio_close(sg_audio);
        if (OPRT_OK != rt) {
            return rt;
        }
        sg_device_open = FALSE;
    }
    return OPRT_OK;
}

OPERATE_RET xz_audio_resume(void)
{
    OPERATE_RET rt = OPRT_OK;
    if (!sg_device_open) {
        TUYA_CALL_ERR_RETURN(tdl_audio_open(sg_audio, __mic_cb));
        sg_device_open = TRUE;
        TUYA_CALL_ERR_RETURN(tdl_audio_volume_set(sg_audio, sg_volume));
    }
    tal_mutex_lock(sg_play_mutex);
    sg_paused = FALSE;
    tal_mutex_unlock(sg_play_mutex);
    uint32_t start = (uint32_t)tal_system_get_millisecond();
    while (sg_pause_ack) {
        if ((uint32_t)((uint32_t)tal_system_get_millisecond() - start) >= 1500U) {
            return OPRT_TIMEOUT;
        }
        tal_system_sleep(10);
    }
    return OPRT_OK;
}
