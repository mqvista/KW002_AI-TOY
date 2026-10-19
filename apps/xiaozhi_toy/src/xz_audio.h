/**
 * @file xz_audio.h
 * @brief Audio service: mic -> Opus encoder -> sender, Opus packets / prompts -> decoder -> speaker.
 */

#ifndef __XZ_AUDIO_H__
#define __XZ_AUDIO_H__

#include "tuya_cloud_types.h"
#include "xz_prompts.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called from the encoder thread with one Opus packet (XZ_OPUS_FRAME_MS of audio). */
typedef void (*XZ_AUDIO_SEND_CB)(const uint8_t *data, uint32_t len);

OPERATE_RET xz_audio_init(void);

/* ---- capture ---- */
/** Start/stop streaming the microphone as Opus packets to cb. */
void xz_audio_capture(BOOL_T on, XZ_AUDIO_SEND_CB cb);

/** Called from the audio driver with every mic frame (16 kHz mono 16 bit PCM after AEC). */
typedef void (*XZ_AUDIO_TAP_CB)(const uint8_t *pcm, uint32_t len);

/** Install a raw PCM tap, independent of capture (used by the wake word). */
void xz_audio_set_mic_tap(XZ_AUDIO_TAP_CB cb);

/* ---- playback ---- */
/** Queue an Opus packet from the server (any sample rate, decoded to 16 kHz). */
void xz_audio_feed_opus(const uint8_t *data, uint32_t len);

/** Queue an embedded prompt sound; with wait, block until it has been played. */
void xz_audio_play_sound(XZ_SOUND_E snd, BOOL_T wait);

/** Speak a string of digits, e.g. an activation code. Blocks until played. */
void xz_audio_play_digits(const char *digits);

/** Speaker volume 0..100 (0 mutes the DAC), applied immediately. */
void xz_audio_set_volume(uint8_t volume);
uint8_t xz_audio_get_volume(void);

/** Drop everything queued and stop the speaker immediately. */
void xz_audio_stop_playback(void);

/** TRUE while packets are queued or audio is still coming out of the speaker. */
BOOL_T xz_audio_is_playing(void);

/** Block until playback is idle or timeout. */
void xz_audio_wait_idle(uint32_t timeout_ms);

/** Quiesce the playback worker and close the codec before deep sleep. */
OPERATE_RET xz_audio_suspend(void);
/** Restore after a cancelled/failed sleep attempt; preserves the selected volume. */
OPERATE_RET xz_audio_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_AUDIO_H__ */
