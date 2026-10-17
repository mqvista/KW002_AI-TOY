/**
 * @file xz_wake.h
 * @brief Offline wake word ("小智同学") on the T5 KWS engine (Tuya TUTUClear model).
 */

#ifndef __XZ_WAKE_H__
#define __XZ_WAKE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called from the KWS thread when the wake word was heard; word is XZ_WAKE_WORD_TEXT. */
typedef void (*XZ_WAKE_CB)(const char *word);

OPERATE_RET xz_wake_init(XZ_WAKE_CB cb);

/** Detection on/off. Off drops the buffered audio, so nothing old triggers later. */
void xz_wake_enable(BOOL_T on);

/** Feed 16 kHz mono 16 bit mic PCM (any length, normally 20 ms frames). */
void xz_wake_feed(const uint8_t *pcm, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_WAKE_H__ */
