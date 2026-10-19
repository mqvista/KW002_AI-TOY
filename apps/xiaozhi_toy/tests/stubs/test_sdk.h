#ifndef TEST_SDK_H
#define TEST_SDK_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int      BOOL_T;
typedef int      OPERATE_RET;
typedef uint64_t SYS_TIME_T;
typedef void    *QUEUE_HANDLE;
typedef void    *MUTEX_HANDLE;
typedef void    *THREAD_HANDLE;
typedef void    *TDL_AUDIO_HANDLE_T;
typedef void    *TDL_POWER_HANDLE;
typedef int      TDL_AUDIO_FRAME_FORMAT_E;
typedef int      TDL_AUDIO_STATUS_E;
typedef struct {
    unsigned    stackDepth;
    int         priority;
    const char *thrdname;
} THREAD_CFG_T;
typedef void (*TEST_MIC_CB)(TDL_AUDIO_FRAME_FORMAT_E, TDL_AUDIO_STATUS_E, uint8_t *, uint32_t);
typedef enum { TDL_CHG_DISCHARGE, TDL_CHG_CHARGING, TDL_CHG_FULL } TDL_CHG_STATE_E;
typedef struct {
    int source;
    union {
        struct {
            int gpio_num;
            int level;
        } gpio_param;
    } wakeup_para;
} TUYA_WAKEUP_SOURCE_BASE_CFG_T;
typedef struct {
    int unused;
} OpusEncoder;
typedef struct {
    int unused;
} OpusDecoder;
typedef int32_t opus_int32;
typedef int16_t opus_int16;
#define TRUE                    1
#define FALSE                   0
#define OPRT_OK                 0
#define OPRT_COM_ERROR          (-1)
#define OPRT_TIMEOUT            (-2)
#define OPRT_RESOURCE_NOT_READY (-3)
#define OPRT_NOT_FOUND          (-4)
#define THREAD_PRIO_2           2
#define AUDIO_CODEC_NAME        "audio_codec"
#define POWER_NAME              "power"
#define BOARD_BUTTON_TALK_PIN   33
#define TUYA_WAKEUP_SOURCE_GPIO 1
#define TUYA_GPIO_WAKEUP_LOW    0
#define OPUS_OK                 0
#define OPUS_RESET_STATE        4028
#define OPUS_APPLICATION_VOIP   2048
#define OPUS_SET_COMPLEXITY(x)  (x)
#define PR_DEBUG(...)           ((void)0)
#define PR_NOTICE(...)          ((void)0)
#define PR_WARN(...)            ((void)0)
#define PR_ERR(...)             ((void)0)
/* Match the SDK macro's requirement for an rt local. */
#define TUYA_CALL_ERR_RETURN(f)                                                                                        \
    do {                                                                                                               \
        rt = (f);                                                                                                      \
        if (rt != OPRT_OK)                                                                                             \
            return rt;                                                                                                 \
    } while (0)
#define TUYA_CALL_ERR_LOG(f)                                                                                           \
    do {                                                                                                               \
        rt = (f);                                                                                                      \
    } while (0)
SYS_TIME_T       tal_system_get_millisecond(void);
void             tal_system_sleep(unsigned);
uint32_t         tal_system_enter_critical(void);
void             tal_system_exit_critical(uint32_t);
void            *tal_malloc(size_t);
void             tal_free(void *);
void            *tkl_system_psram_malloc(size_t);
OPERATE_RET      tal_mutex_create_init(MUTEX_HANDLE *);
OPERATE_RET      tal_mutex_lock(MUTEX_HANDLE);
OPERATE_RET      tal_mutex_unlock(MUTEX_HANDLE);
OPERATE_RET      tal_queue_create_init(QUEUE_HANDLE *, unsigned, unsigned);
OPERATE_RET      tal_queue_post(QUEUE_HANDLE, void *, unsigned);
OPERATE_RET      tal_queue_fetch(QUEUE_HANDLE, void *, unsigned);
OPERATE_RET      tal_thread_create_and_start(THREAD_HANDLE *, void *, void *, void (*)(void *), void *,
                                             const THREAD_CFG_T *);
OPERATE_RET      board_speaker_set(BOOL_T);
BOOL_T           board_sleep_inputs_ready(void);
void             board_prepare_sleep_pins(void);
OPERATE_RET      tdl_audio_find(const char *, TDL_AUDIO_HANDLE_T *);
OPERATE_RET      tdl_audio_open(TDL_AUDIO_HANDLE_T, TEST_MIC_CB);
OPERATE_RET      tdl_audio_close(TDL_AUDIO_HANDLE_T);
OPERATE_RET      tdl_audio_play(TDL_AUDIO_HANDLE_T, uint8_t *, uint32_t);
OPERATE_RET      tdl_audio_play_stop(TDL_AUDIO_HANDLE_T);
OPERATE_RET      tdl_audio_volume_set(TDL_AUDIO_HANDLE_T, uint8_t);
TDL_POWER_HANDLE tdl_power_find(const char *);
OPERATE_RET      tdl_power_charger_get_state(TDL_POWER_HANDLE, TDL_CHG_STATE_E *);
OPERATE_RET      tdl_power_enter_deepsleep(TDL_POWER_HANDLE, uint32_t);
OPERATE_RET      tkl_wakeup_source_set(const TUYA_WAKEUP_SOURCE_BASE_CFG_T *);
OPERATE_RET      tkl_sleep_register_ds_user_cb(void (*)(void));
OpusEncoder     *opus_encoder_create(int, int, int, int *);
OpusDecoder     *opus_decoder_create(int, int, int *);
int              opus_encode(OpusEncoder *, const opus_int16 *, int, uint8_t *, int);
int              opus_decode(OpusDecoder *, const uint8_t *, int, int16_t *, int, int);
int              opus_encoder_ctl(OpusEncoder *, int);
int              opus_decoder_ctl(OpusDecoder *, int);
#endif
