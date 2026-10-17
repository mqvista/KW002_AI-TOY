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
static OpusEncoder *sg_encoder = NULL;

static QUEUE_HANDLE sg_play_queue = NULL;
static OpusDecoder *sg_decoder = NULL;
static MUTEX_HANDLE sg_play_mutex = NULL;
static volatile uint32_t sg_play_pending = 0;   /* packets queued, not yet written */
static volatile SYS_TIME_T sg_play_end_ms = 0;  /* projected end of what was written */
static volatile BOOL_T sg_flush = FALSE;

static THREAD_HANDLE sg_enc_thread = NULL;
static THREAD_HANDLE sg_play_thread = NULL;

/***********************************************************
***********************function define**********************
***********************************************************/
static void __mic_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status, uint8_t *data, uint32_t len)
{
    (void)type;
    (void)status;
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

static void __play_task(void *arg)
{
    int16_t *pcm = tkl_system_psram_malloc(DEC_MAX_SAMPLES * sizeof(int16_t));
    PLAY_PKT_T *pkt = NULL;

    (void)arg;
    if (!pcm) {
        PR_ERR("decoder buffer alloc failed");
        return;
    }

    while (1) {
        if (sg_flush) {
            __play_free_all();
            tdl_audio_play_stop(sg_audio);
            opus_decoder_ctl(sg_decoder, OPUS_RESET_STATE);
            sg_play_end_ms = tal_system_get_millisecond();
            sg_flush = FALSE;
        }

        if (OPRT_OK != tal_queue_fetch(sg_play_queue, &pkt, 50)) {
            continue;
        }

        if (pkt->len == 0) {
            opus_decoder_ctl(sg_decoder, OPUS_RESET_STATE);
        } else if (!sg_flush) {
            int samples = opus_decode(sg_decoder, pkt->data, pkt->len, pcm, DEC_MAX_SAMPLES, 0);
            if (samples > 0) {
                SYS_TIME_T now = tal_system_get_millisecond();
                uint32_t dur = (uint32_t)samples * 1000 / XZ_SAMPLE_RATE;
                if (sg_play_end_ms < now) {
                    sg_play_end_ms = now;
                }
                sg_play_end_ms += dur;
                tdl_audio_play(sg_audio, (uint8_t *)pcm, (uint32_t)samples * sizeof(int16_t));
            } else {
                PR_WARN("opus_decode error %d (len %u)", samples, pkt->len);
            }
        }
        tal_free(pkt);
        __pending_add(-1);
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
    __pending_add(1);
    if (OPRT_OK != tal_queue_post(sg_play_queue, &pkt, 0)) {
        __pending_add(-1);
        tal_free(pkt);
        PR_WARN("play queue full, packet dropped");
    }
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
    TUYA_CALL_ERR_LOG(tdl_audio_volume_set(sg_audio, XZ_SPEAKER_VOLUME));

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

void xz_audio_feed_opus(const uint8_t *data, uint32_t len)
{
    if (len == 0 || len > 0xFFFF) {
        return;
    }
    __play_push(data, (uint16_t)len);
}

BOOL_T xz_audio_is_playing(void)
{
    return sg_play_pending > 0 || sg_flush || tal_system_get_millisecond() < sg_play_end_ms;
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
