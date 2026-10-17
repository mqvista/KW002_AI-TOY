/**
 * @file xz_prompts.h
 * @brief Embedded prompt sounds (Ogg/Opus, 16 kHz mono).
 */

#ifndef __XZ_PROMPTS_H__
#define __XZ_PROMPTS_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Order must match tools/gen_prompts.py output (sorted file names). */
typedef enum {
    XZ_SND_0,
    XZ_SND_1,
    XZ_SND_2,
    XZ_SND_3,
    XZ_SND_4,
    XZ_SND_5,
    XZ_SND_6,
    XZ_SND_7,
    XZ_SND_8,
    XZ_SND_9,
    XZ_SND_ACTIVATION,  /* "please add the device on xiaozhi.me with code ..." */
    XZ_SND_CHARGER_REMOVED, /* "充电器已拔出" (tools/tts_prompts.py) */
    XZ_SND_CHARGING,    /* "开始充电" (tools/tts_prompts.py) */
    XZ_SND_CONNECT_AP,  /* "请用手机连接热点，小智杠" (tools/tts_prompts.py) */
    /* SSID characters 0-9, A-F in the same voice, contiguous: XZ_SND_SSID_0 + value */
    XZ_SND_SSID_0,
    XZ_SND_SSID_1,
    XZ_SND_SSID_2,
    XZ_SND_SSID_3,
    XZ_SND_SSID_4,
    XZ_SND_SSID_5,
    XZ_SND_SSID_6,
    XZ_SND_SSID_7,
    XZ_SND_SSID_8,
    XZ_SND_SSID_9,
    XZ_SND_SSID_A,
    XZ_SND_SSID_B,
    XZ_SND_SSID_C,
    XZ_SND_SSID_D,
    XZ_SND_SSID_E,
    XZ_SND_SSID_F,
    XZ_SND_EXCLAMATION, /* error */
    XZ_SND_LOW_BATTERY,
    XZ_SND_NET_FAILED,  /* "网络连接失败" (tools/tts_prompts.py) */
    XZ_SND_NET_LOST,    /* "网络已断开" (tools/tts_prompts.py) */
    XZ_SND_NET_RESTORED,/* "网络已恢复" (tools/tts_prompts.py) */
    XZ_SND_POPUP,       /* short notification */
    XZ_SND_SUCCESS,     /* ready */
    XZ_SND_WELCOME,
    XZ_SND_WIFICONFIG,  /* entering Wi-Fi configuration mode */
    XZ_SND_MAX,
} XZ_SOUND_E;

typedef struct {
    const uint8_t *data;
    size_t len;
} XZ_SOUND_T;

extern const XZ_SOUND_T g_xz_sounds[XZ_SND_MAX];

#ifdef __cplusplus
}
#endif

#endif /* __XZ_PROMPTS_H__ */
