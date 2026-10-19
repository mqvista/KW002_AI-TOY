/* Exercise the real playback iteration and standby policy with fake hardware. */
#include <assert.h>
#include "test_sdk.h"
#include "../src/xz_audio.c"
#include "../src/xz_power.c"

static uint32_t        clock_ms, write_delay;
static int             pa_on, pa_opens, pa_closes, writes, closes, opens, sleep_calls;
static int             fail_pa, fail_write, fail_wake, fail_charger, fail_close, fail_open;
static BOOL_T          inputs_ready  = TRUE, worker_on_sleep, worker_in_step, touch_on_critical;
static TDL_CHG_STATE_E charger_state = TDL_CHG_DISCHARGE;
static int16_t         pcm[DEC_MAX_SAMPLES];
static void           *queue[PLAY_QUEUE_DEPTH];
static unsigned        q_read, q_write, q_count;
static unsigned        tests;
const XZ_SOUND_T       g_xz_sounds[XZ_SND_MAX] = {{0}};

SYS_TIME_T tal_system_get_millisecond(void)
{
    return clock_ms;
}
void tal_system_sleep(unsigned ms)
{
    clock_ms += ms;
    if (worker_on_sleep && !worker_in_step) {
        worker_in_step = TRUE;
        __play_step(pcm);
        worker_in_step = FALSE;
    }
}
uint32_t tal_system_enter_critical(void)
{
    if (touch_on_critical) {
        touch_on_critical = FALSE;
        clock_ms++;
        sg_last_activity = clock_ms;
        sg_generation++;
    }
    return 0;
}
void  tal_system_exit_critical(uint32_t mask) {}
void *tal_malloc(size_t n)
{
    return malloc(n);
}
void tal_free(void *p)
{
    free(p);
}
void *tkl_system_psram_malloc(size_t n)
{
    return malloc(n);
}
OPERATE_RET tal_mutex_create_init(MUTEX_HANDLE *m)
{
    *m = (void *)1;
    return 0;
}
OPERATE_RET tal_mutex_lock(MUTEX_HANDLE m)
{
    return 0;
}
OPERATE_RET tal_mutex_unlock(MUTEX_HANDLE m)
{
    return 0;
}
OPERATE_RET tal_queue_create_init(QUEUE_HANDLE *q, unsigned size, unsigned count)
{
    *q = (void *)1;
    return 0;
}
OPERATE_RET tal_queue_post(QUEUE_HANDLE q, void *p, unsigned ms)
{
    if (q_count == PLAY_QUEUE_DEPTH)
        return -1;
    queue[q_write] = *(void **)p;
    q_write        = (q_write + 1) % PLAY_QUEUE_DEPTH;
    q_count++;
    return 0;
}
OPERATE_RET tal_queue_fetch(QUEUE_HANDLE q, void *p, unsigned ms)
{
    if (!q_count) {
        clock_ms += ms;
        return -1;
    }
    *(void **)p = queue[q_read];
    q_read      = (q_read + 1) % PLAY_QUEUE_DEPTH;
    q_count--;
    return 0;
}
OPERATE_RET tal_thread_create_and_start(THREAD_HANDLE *h, void *a, void *b, void (*f)(void *), void *v,
                                        const THREAD_CFG_T *c)
{
    return 0;
}
OPERATE_RET board_speaker_set(BOOL_T on)
{
    if (fail_pa)
        return -1;
    pa_on = on;
    if (on)
        pa_opens++;
    else
        pa_closes++;
    return 0;
}
BOOL_T board_sleep_inputs_ready(void)
{
    return inputs_ready;
}
void        board_prepare_sleep_pins(void) {}
OPERATE_RET tdl_audio_find(const char *n, TDL_AUDIO_HANDLE_T *h)
{
    *h = (void *)1;
    return 0;
}
OPERATE_RET tdl_audio_open(TDL_AUDIO_HANDLE_T h, TEST_MIC_CB cb)
{
    opens++;
    return fail_open ? -1 : 0;
}
OPERATE_RET tdl_audio_close(TDL_AUDIO_HANDLE_T h)
{
    assert(sg_pause_ack);
    closes++;
    return fail_close ? -1 : 0;
}
OPERATE_RET tdl_audio_play(TDL_AUDIO_HANDLE_T h, uint8_t *p, uint32_t n)
{
    assert(pa_on || sg_volume == 0);
    assert(!sg_paused);
    writes++;
    clock_ms += write_delay;
    return fail_write ? -1 : 0;
}
OPERATE_RET tdl_audio_play_stop(TDL_AUDIO_HANDLE_T h)
{
    return 0;
}
OPERATE_RET tdl_audio_volume_set(TDL_AUDIO_HANDLE_T h, uint8_t v)
{
    return 0;
}
TDL_POWER_HANDLE tdl_power_find(const char *n)
{
    return (void *)1;
}
OPERATE_RET tdl_power_charger_get_state(TDL_POWER_HANDLE h, TDL_CHG_STATE_E *s)
{
    *s = charger_state;
    return fail_charger ? -1 : 0;
}
OPERATE_RET tdl_power_enter_deepsleep(TDL_POWER_HANDLE h, uint32_t t)
{
    assert(t == 0);
    sleep_calls++;
    return -9;
}
OPERATE_RET tkl_wakeup_source_set(const TUYA_WAKEUP_SOURCE_BASE_CFG_T *w)
{
    assert(w->source == TUYA_WAKEUP_SOURCE_GPIO && w->wakeup_para.gpio_param.gpio_num == 33);
    return fail_wake ? -1 : 0;
}
OPERATE_RET tkl_sleep_register_ds_user_cb(void (*cb)(void))
{
    assert(cb == board_prepare_sleep_pins);
    return 0;
}
OpusEncoder *opus_encoder_create(int a, int b, int c, int *e)
{
    static OpusEncoder o;
    *e = 0;
    return &o;
}
OpusDecoder *opus_decoder_create(int a, int b, int *e)
{
    static OpusDecoder o;
    *e = 0;
    return &o;
}
int opus_encode(OpusEncoder *o, const opus_int16 *p, int n, uint8_t *b, int max)
{
    return 1;
}
int opus_decode(OpusDecoder *o, const uint8_t *p, int n, int16_t *b, int max, int f)
{
    return 960;
}
int opus_encoder_ctl(OpusEncoder *o, int req)
{
    return 0;
}
int opus_decoder_ctl(OpusDecoder *o, int req)
{
    return 0;
}

static void reset(void)
{
    while (q_count) {
        void *p;
        tal_queue_fetch(0, &p, 0);
        free(p);
    }
    q_read = q_write = q_count = 0;
    clock_ms                   = 1000;
    write_delay                = 0;
    pa_on = pa_opens = pa_closes = writes = closes = opens = sleep_calls = 0;
    fail_pa = fail_write = fail_wake = fail_charger = fail_close = fail_open = 0;
    inputs_ready                                                             = TRUE;
    worker_on_sleep = worker_in_step = touch_on_critical = FALSE;
    charger_state                                        = TDL_CHG_DISCHARGE;
    sg_play_pending                                      = 0;
    sg_play_end_ms                                       = 0;
    sg_play_end_valid                                    = FALSE;
    sg_flush = sg_pa_on = sg_paused = sg_pause_ack = FALSE;
    sg_volume                                      = 100;
    sg_device_open                                 = TRUE;
    sg_play_mutex                                  = (void *)1;
    sg_audio                                       = (void *)1;
    sg_capture_on                                  = FALSE;
    sg_retry_wait                                  = FALSE;
    assert(xz_power_init() == 0);
}
static void packet(void)
{
    uint8_t data = 1;
    xz_audio_feed_opus(&data, 1);
}
static void passed(const char *name)
{
    printf("PASS %s\n", name);
    tests++;
}

int main(void)
{
    reset();
    packet();
    assert(!pa_on);
    __play_step(pcm);
    assert(pa_on && writes == 1 && pa_opens == 1 && sg_play_pending == 0);
    clock_ms = (uint32_t)sg_play_end_ms + XZ_PA_TAIL_MS - 1;
    __speaker_idle_poll();
    assert(pa_on);
    clock_ms++;
    __speaker_idle_poll();
    assert(!pa_on && !xz_audio_is_playing());
    passed("first frame warmup and complete tail before PA off");

    reset();
    packet();
    __play_step(pcm);
    clock_ms = (uint32_t)sg_play_end_ms + 10;
    packet();
    __play_step(pcm);
    assert(pa_opens == 1 && pa_closes == 0 && writes == 2);
    passed("adjacent prompts keep PA enabled");

    reset();
    sg_volume = 0;
    packet();
    __play_step(pcm);
    assert(!pa_on && writes == 1);
    sg_volume = 60;
    __speaker_idle_poll();
    assert(pa_on);
    sg_volume = 0;
    __speaker_idle_poll();
    assert(!pa_on);
    passed("mute and unmute during buffered playback");

    reset();
    clock_ms = UINT32_MAX - 30;
    packet();
    __play_step(pcm);
    assert(pa_on);
    clock_ms = (uint32_t)sg_play_end_ms + XZ_PA_TAIL_MS;
    __speaker_idle_poll();
    assert(!pa_on);
    passed("playback drain across millisecond rollover");

    reset();
    write_delay = 500;
    packet();
    __play_step(pcm);
    assert(pa_on && (int32_t)((uint32_t)sg_play_end_ms - clock_ms) > 0);
    passed("blocked write retains PA for newly accepted PCM");

    reset();
    fail_write = 1;
    packet();
    packet();
    __play_step(pcm);
    assert(!pa_on && !q_count && sg_play_pending == 0 && !xz_audio_is_playing());
    passed("write failure drops pending audio and closes PA");

    reset();
    fail_pa = 1;
    packet();
    __play_step(pcm);
    assert(!pa_on && writes == 0);
    passed("PA enable failure prevents PCM submission");

    reset();
    packet();
    __play_step(pcm);
    sg_flush = TRUE;
    __play_step(pcm);
    assert(!pa_on && !xz_audio_is_playing());
    passed("abort closes PA");

    reset();
    worker_on_sleep = TRUE;
    assert(xz_audio_suspend() == 0);
    assert(sg_pause_ack && sg_paused && !sg_device_open && closes == 1);
    packet();
    assert(!q_count);
    assert(xz_audio_resume() == 0);
    worker_on_sleep = FALSE;
    assert(sg_device_open && opens == 1 && sg_volume == 100);
    __play_step(pcm);
    packet();
    __play_step(pcm);
    assert(writes == 1 && pa_on);
    passed("suspend handshake rejects late packets and restores playback");

    reset();
    assert(xz_audio_suspend() == OPRT_TIMEOUT);
    assert(closes == 0);
    assert(xz_audio_resume() == 0 && !sg_paused);
    passed("unresponsive worker never races codec close");

    reset();
    clock_ms += XZ_IDLE_SLEEP_MS - 1;
    assert(!xz_power_should_sleep(TRUE));
    clock_ms++;
    assert(xz_power_should_sleep(TRUE));
    assert(xz_power_enter_sleep() == -9 && sleep_calls == 1);
    passed("five-minute boundary and P33-only sleep entry");

    reset();
    clock_ms += XZ_IDLE_SLEEP_MS;
    charger_state = TDL_CHG_CHARGING;
    assert(!xz_power_should_sleep(TRUE));
    charger_state = TDL_CHG_DISCHARGE;
    clock_ms += XZ_IDLE_SLEEP_MS - 1;
    assert(!xz_power_should_sleep(TRUE));
    clock_ms++;
    assert(xz_power_should_sleep(TRUE));
    passed("charging inhibits sleep; unplug starts full idle interval");

    reset();
    clock_ms += XZ_IDLE_SLEEP_MS;
    inputs_ready = FALSE;
    assert(!xz_power_should_sleep(TRUE));
    inputs_ready = TRUE;
    clock_ms += XZ_IDLE_SLEEP_MS;
    assert(xz_power_should_sleep(TRUE));
    xz_power_activity();
    assert(xz_power_enter_sleep() == OPRT_RESOURCE_NOT_READY && !sleep_calls);
    passed("held keys and late interaction cancel sleep");

    reset();
    clock_ms += XZ_IDLE_SLEEP_MS;
    assert(xz_power_should_sleep(TRUE));
    inputs_ready = FALSE;
    assert(xz_power_enter_sleep() == OPRT_RESOURCE_NOT_READY && !sleep_calls);
    passed("charger/key change during teardown cancels sleep");

    reset();
    clock_ms += XZ_IDLE_SLEEP_MS;
    fail_charger = 1;
    assert(!xz_power_should_sleep(TRUE));
    fail_charger = 0;
    clock_ms += XZ_IDLE_SLEEP_MS;
    assert(xz_power_should_sleep(TRUE));
    fail_wake = 1;
    assert(xz_power_enter_sleep() != 0 && !sleep_calls);
    passed("charger read and wake configuration failures inhibit sleep");

    reset();
    clock_ms += XZ_IDLE_SLEEP_MS;
    assert(xz_power_should_sleep(TRUE));
    xz_power_defer();
    clock_ms += XZ_SLEEP_RETRY_MS - 1;
    assert(!xz_power_should_sleep(TRUE));
    clock_ms++;
    assert(xz_power_should_sleep(TRUE));
    passed("failed preparation is rate limited");

    reset();
    clock_ms = UINT32_MAX - 100;
    xz_power_activity();
    clock_ms += XZ_IDLE_SLEEP_MS;
    assert(xz_power_should_sleep(TRUE));
    passed("standby timeout across millisecond rollover");

    reset();
    clock_ms += XZ_IDLE_SLEEP_MS;
    assert(!xz_power_should_sleep(FALSE));
    clock_ms += XZ_IDLE_SLEEP_MS - 1;
    assert(!xz_power_should_sleep(TRUE));
    packet();
    assert(!xz_power_should_sleep(TRUE));
    passed("active conversation and queued prompt postpone sleep");
    reset();
    worker_on_sleep = TRUE;
    for (int i = 0; i < 3; i++) {
        assert(xz_audio_suspend() == 0);
        assert(xz_audio_resume() == 0);
        assert(!sg_pause_ack);
    }
    worker_on_sleep = FALSE;
    assert(closes == 3 && opens == 3);
    passed("repeated suspend/resume cannot reuse a stale acknowledgement");

    reset();
    worker_on_sleep = TRUE;
    fail_close      = 1;
    assert(xz_audio_suspend() != 0 && sg_device_open);
    assert(xz_audio_resume() == 0);
    worker_on_sleep = FALSE;
    assert(!sg_paused);
    passed("codec close failure restores an accepting playback worker");

    reset();
    clock_ms += XZ_IDLE_SLEEP_MS;
    touch_on_critical = TRUE;
    assert(!xz_power_should_sleep(TRUE) && !sleep_calls);
    passed("interaction between clock read and timeout decision prevents premature sleep");

    printf("%u regression cases passed\n", tests);
    return 0;
}
