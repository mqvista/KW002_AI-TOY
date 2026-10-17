/**
 * @file app_main.c
 * @brief KW002 XiaoZhi AI toy: boot flow, Wi-Fi provisioning, activation and conversation state machine.
 *
 * Keys: P33 talk  - press: start chat / interrupt the reply / end chat
 *       P12 reset - hold 3 s: "entering Wi-Fi configuration mode", reboot into the SoftAP portal
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "cJSON.h"
#include "tdl_button_manage.h"
#include "tuya_tls.h"
#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
#include "lwip_init.h"
#endif

#include "board_com_api.h"

#include "xz_audio.h"
#include "xz_config.h"
#include "xz_ota.h"
#include "xz_portal.h"
#include "xz_settings.h"
#include "xz_time.h"
#include "xz_ui.h"
#include "xz_wifi.h"
#include "xz_ws.h"

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef enum {
    EV_KEY_TALK,
    EV_KEY_RESET_LONG,
    EV_WS_TEXT,   /* text: malloc'd JSON, freed by the handler */
    EV_WS_CLOSED,
    EV_WIFI_DOWN,
} APP_EV_E;

typedef struct {
    APP_EV_E type;
    char *text;
} APP_EV_T;

typedef enum {
    ST_BOOT,
    ST_PORTAL,
    ST_IDLE,
    ST_OPENING,   /* websocket connected, waiting for the server hello */
    ST_LISTENING,
    ST_SPEAKING,
    ST_OFFLINE,
} APP_STATE_E;

static const char *sg_state_name[] = {"boot", "portal", "idle", "opening", "listening", "speaking", "offline"};

/***********************************************************
***********************variable define**********************
***********************************************************/
static QUEUE_HANDLE sg_ev_queue = NULL;
static volatile APP_STATE_E sg_state = ST_BOOT;
static XZ_OTA_INFO_T sg_ota;
static char sg_session_id[64] = {0};
static BOOL_T sg_tts_stopped = FALSE;
static SYS_TIME_T sg_state_since = 0;
static char sg_auth[192];

/***********************************************************
***********************function define**********************
***********************************************************/
static void __post_event(APP_EV_E type, char *text)
{
    APP_EV_T ev = {.type = type, .text = text};
    if (OPRT_OK != tal_queue_post(sg_ev_queue, &ev, 0)) {
        tal_free(text);
    }
}

static void __set_state(APP_STATE_E st)
{
    if (st == sg_state) {
        return;
    }
    PR_NOTICE("state %s -> %s", sg_state_name[sg_state], sg_state_name[st]);
    sg_state = st;
    sg_state_since = tal_system_get_millisecond();

    switch (st) {
    case ST_PORTAL:
        xz_ui_set(XZ_UI_PORTAL);
        break;
    case ST_OPENING:
    case ST_BOOT:
        xz_ui_set(XZ_UI_CONNECTING);
        break;
    case ST_LISTENING:
        xz_ui_set(XZ_UI_LISTENING);
        break;
    case ST_SPEAKING:
        xz_ui_set(XZ_UI_SPEAKING);
        break;
    case ST_OFFLINE:
        xz_ui_set(XZ_UI_ERROR);
        break;
    case ST_IDLE:
    default:
        xz_ui_set(XZ_UI_OFF);
        break;
    }
}

/* ---------------------------------------------------------------- keys */

static void __talk_key_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *arg)
{
    (void)name;
    (void)arg;
    if (TDL_BUTTON_PRESS_DOWN == event) {
        __post_event(EV_KEY_TALK, NULL);
    }
}

static void __reset_key_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *arg)
{
    (void)name;
    (void)arg;
    if (TDL_BUTTON_LONG_PRESS_START == event) {
        __post_event(EV_KEY_RESET_LONG, NULL);
    }
}

static void __keys_init(void)
{
    TDL_BUTTON_CFG_T cfg = {
        .long_start_valid_time = 3000,
        .long_keep_timer = 1000,
        .button_debounce_time = 50,
        .button_repeat_valid_count = 0,
        .button_repeat_valid_time = 300,
    };
    TDL_BUTTON_HANDLE talk = NULL, reset = NULL;

    if (OPRT_OK == tdl_button_create(BUTTON_NAME, &cfg, &talk)) {
        tdl_button_event_register(talk, TDL_BUTTON_PRESS_DOWN, __talk_key_cb);
    }
    if (OPRT_OK == tdl_button_create(BUTTON_NAME_2, &cfg, &reset)) {
        tdl_button_event_register(reset, TDL_BUTTON_LONG_PRESS_START, __reset_key_cb);
    }
}

/* ---------------------------------------------------------------- provisioning */

/* Long press on the reset key: announce, remember, reboot into the portal. */
static void __enter_portal_by_key(void)
{
    PR_NOTICE("reset key held: entering Wi-Fi configuration mode");
    xz_ws_close();
    xz_audio_capture(FALSE, NULL);
    xz_audio_stop_playback();
    xz_ui_set(XZ_UI_PORTAL);
    xz_audio_play_sound(XZ_SND_WIFICONFIG, TRUE);
    xz_settings_set_flag(XZ_FLAG_FORCE_PORTAL, TRUE);
    tal_system_sleep(200);
    tal_system_reset();
}

static void __run_portal(BOOL_T announce)
{
    __set_state(ST_PORTAL);
    if (announce) {
        xz_audio_play_sound(XZ_SND_WIFICONFIG, TRUE);
    }
    if (OPRT_OK != xz_portal_run()) {
        PR_ERR("portal start failed, rebooting");
        tal_system_sleep(2000);
        tal_system_reset();
    }
    /* the portal reboots the device once credentials are saved */
    while (1) {
        APP_EV_T ev;
        if (OPRT_OK == tal_queue_fetch(sg_ev_queue, &ev, 1000)) {
            tal_free(ev.text);
        }
    }
}

/* Sleep while still reacting to the reset key; other events are dropped. */
static void __sleep_events(uint32_t ms)
{
    SYS_TIME_T deadline = tal_system_get_millisecond() + ms;

    while (tal_system_get_millisecond() < deadline) {
        APP_EV_T ev;
        if (OPRT_OK == tal_queue_fetch(sg_ev_queue, &ev, 100)) {
            tal_free(ev.text);
            if (EV_KEY_RESET_LONG == ev.type) {
                __enter_portal_by_key();
            }
        }
    }
}

/* ---------------------------------------------------------------- network bring-up */

static void __wifi_link_cb(BOOL_T up)
{
    if (!up && sg_state != ST_BOOT && sg_state != ST_PORTAL) {
        __post_event(EV_WIFI_DOWN, NULL);
    }
}

static void __connect_wifi(void)
{
    char ssid[XZ_SSID_MAX + 1] = {0};
    char passwd[XZ_PASSWD_MAX + 1] = {0};
    uint32_t backoff = 5000;

    xz_settings_get_wifi(ssid, passwd);
    xz_ui_set(XZ_UI_CONNECTING);

    while (OPRT_OK != xz_wifi_connect(ssid, passwd, XZ_WIFI_CONNECT_TIMEOUT_MS)) {
        if (xz_settings_get_flag(XZ_FLAG_WIFI_UNTESTED)) {
            /* just provisioned and never connected: probably a wrong password */
            PR_ERR("new wifi credentials do not work, back to the portal");
            xz_settings_set_flag(XZ_FLAG_WIFI_UNTESTED, FALSE);
            xz_audio_play_sound(XZ_SND_EXCLAMATION, TRUE);
            xz_settings_set_flag(XZ_FLAG_FORCE_PORTAL, TRUE);
            tal_system_reset();
        }
        xz_ui_set(XZ_UI_ERROR);
        xz_audio_play_sound(XZ_SND_EXCLAMATION, TRUE);
        PR_WARN("wifi not available, retry in %u s (hold the reset key 3 s to reconfigure)", backoff / 1000);
        __sleep_events(backoff);
        backoff = backoff < 60000 ? backoff * 2 : 60000;
        xz_ui_set(XZ_UI_CONNECTING);
    }
    xz_settings_set_flag(XZ_FLAG_WIFI_UNTESTED, FALSE);
}

static void __check_in_and_activate(void)
{
    uint32_t backoff = 5000;

    while (1) {
        OPERATE_RET rt = xz_ota_check(&sg_ota);
        if (sg_ota.server_time_ms) {
            xz_time_set_ms(sg_ota.server_time_ms);
        }
        if (OPRT_OK != rt) {
            xz_ui_set(XZ_UI_ERROR);
            xz_audio_play_sound(XZ_SND_EXCLAMATION, TRUE);
            PR_WARN("check-in failed (%d), retry in %u s", rt, backoff / 1000);
            __sleep_events(backoff);
            backoff = backoff < 60000 ? backoff * 2 : 60000;
            xz_ui_set(XZ_UI_CONNECTING);
            continue;
        }
        backoff = 5000;
        if (!sg_ota.has_activation) {
            return; /* activated, websocket config in sg_ota */
        }

        /* Activation: speak the code, then poll until the user entered it on xiaozhi.me */
        xz_ui_set(XZ_UI_ACTIVATING);
        PR_NOTICE("==== activation code %s: add the device on xiaozhi.me ====", sg_ota.act_code);
        xz_audio_play_sound(XZ_SND_ACTIVATION, TRUE);
        xz_audio_play_digits(sg_ota.act_code);

        for (int i = 0; i < 10; i++) {
            rt = xz_ota_activate();
            if (OPRT_OK == rt) {
                PR_NOTICE("activation done");
                break;
            }
            __sleep_events(OPRT_RESOURCE_NOT_READY == rt ? 3000 : 10000);
        }
        /* check in again: either activated now, or a fresh announcement of the code */
    }
}

/* ---------------------------------------------------------------- conversation */

static void __ws_on_text(const char *text, uint32_t len)
{
    char *copy = tal_malloc(len + 1);
    if (copy) {
        memcpy(copy, text, len + 1);
        __post_event(EV_WS_TEXT, copy);
    }
}

static void __ws_on_binary(const uint8_t *data, uint32_t len)
{
    /* Opus TTS packets, protocol version 1: raw packets */
    if (sg_state == ST_SPEAKING || sg_state == ST_LISTENING) {
        xz_audio_feed_opus(data, len);
    }
}

static void __ws_on_closed(void)
{
    __post_event(EV_WS_CLOSED, NULL);
}

static void __mic_send_cb(const uint8_t *data, uint32_t len)
{
    xz_ws_send_binary(data, len);
}

static void __send_json(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    PR_DEBUG(">> %s", buf);
    xz_ws_send_text(buf);
}

static void __start_listening(void)
{
    sg_tts_stopped = FALSE;
    __send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}", sg_session_id);
    xz_audio_capture(TRUE, __mic_send_cb);
    __set_state(ST_LISTENING);
}

static void __close_channel(void)
{
    xz_audio_capture(FALSE, NULL);
    xz_ws_close();
    sg_session_id[0] = '\0';
    __set_state(ST_IDLE);
}

static void __open_channel(void)
{
    char ver[4];
    XZ_WS_HEADER_T hdr[4];
    XZ_WS_CB_T cb = {.on_text = __ws_on_text, .on_binary = __ws_on_binary, .on_closed = __ws_on_closed};

    if (strchr(sg_ota.ws_token, ' ')) {
        snprintf(sg_auth, sizeof(sg_auth), "%s", sg_ota.ws_token);
    } else {
        snprintf(sg_auth, sizeof(sg_auth), "Bearer %s", sg_ota.ws_token);
    }
    snprintf(ver, sizeof(ver), "%d", XZ_PROTOCOL_VERSION);
    hdr[0] = (XZ_WS_HEADER_T){"Authorization", sg_auth};
    hdr[1] = (XZ_WS_HEADER_T){"Protocol-Version", ver};
    hdr[2] = (XZ_WS_HEADER_T){"Device-Id", xz_wifi_mac_str()};
    hdr[3] = (XZ_WS_HEADER_T){"Client-Id", xz_settings_uuid()};

    __set_state(ST_OPENING);
    if (OPRT_OK != xz_ws_connect(sg_ota.ws_url, hdr, 4, &cb)) {
        xz_audio_play_sound(XZ_SND_EXCLAMATION, FALSE);
        __set_state(ST_IDLE);
        return;
    }
    __send_json("{\"type\":\"hello\",\"version\":%d,\"features\":{},\"transport\":\"websocket\","
                "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,\"channels\":1,\"frame_duration\":%d}}",
                XZ_PROTOCOL_VERSION, XZ_SAMPLE_RATE, XZ_OPUS_FRAME_MS);
}

static void __handle_server_json(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    cJSON *type = root ? cJSON_GetObjectItem(root, "type") : NULL;

    if (!cJSON_IsString(type)) {
        PR_WARN("<< %s", text);
        cJSON_Delete(root);
        return;
    }
    const char *t = type->valuestring;

    if (strcmp(t, "hello") == 0) {
        cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid)) {
            snprintf(sg_session_id, sizeof(sg_session_id), "%s", sid->valuestring);
        }
        PR_NOTICE("server hello, session %s", sg_session_id);
        if (sg_state == ST_OPENING) {
            __start_listening();
        }
    } else if (strcmp(t, "stt") == 0) {
        cJSON *txt = cJSON_GetObjectItem(root, "text");
        PR_NOTICE("[user] %s", cJSON_IsString(txt) ? txt->valuestring : "");
        sg_state_since = tal_system_get_millisecond();
    } else if (strcmp(t, "llm") == 0) {
        cJSON *emo = cJSON_GetObjectItem(root, "emotion");
        PR_DEBUG("[emotion] %s", cJSON_IsString(emo) ? emo->valuestring : "");
    } else if (strcmp(t, "tts") == 0) {
        cJSON *st = cJSON_GetObjectItem(root, "state");
        const char *s = cJSON_IsString(st) ? st->valuestring : "";
        if (strcmp(s, "start") == 0) {
            xz_audio_capture(FALSE, NULL);
            sg_tts_stopped = FALSE;
            __set_state(ST_SPEAKING);
        } else if (strcmp(s, "sentence_start") == 0) {
            cJSON *txt = cJSON_GetObjectItem(root, "text");
            PR_NOTICE("[xiaozhi] %s", cJSON_IsString(txt) ? txt->valuestring : "");
        } else if (strcmp(s, "stop") == 0) {
            sg_tts_stopped = TRUE; /* resume listening once playback drained */
        }
    } else {
        PR_DEBUG("<< %s", text);
    }
    cJSON_Delete(root);
}

static void __on_talk_key(void)
{
    switch (sg_state) {
    case ST_IDLE:
        __open_channel();
        break;
    case ST_LISTENING:
        __send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}", sg_session_id);
        __close_channel();
        break;
    case ST_SPEAKING:
        __send_json("{\"session_id\":\"%s\",\"type\":\"abort\"}", sg_session_id);
        xz_audio_stop_playback();
        __start_listening();
        break;
    default:
        break;
    }
}

static void __on_wifi_down(void)
{
    PR_WARN("wifi link down");
    xz_audio_capture(FALSE, NULL);
    xz_ws_close();
    __set_state(ST_OFFLINE);
}

static void __periodic(void)
{
    SYS_TIME_T now = tal_system_get_millisecond();

    switch (sg_state) {
    case ST_OPENING:
        if (now - sg_state_since > XZ_WS_HELLO_TIMEOUT_MS) {
            PR_ERR("no server hello");
            xz_audio_play_sound(XZ_SND_EXCLAMATION, FALSE);
            __close_channel();
        }
        break;
    case ST_SPEAKING:
        if (sg_tts_stopped && !xz_audio_is_playing()) {
            __start_listening(); /* auto mode: keep the conversation going */
        }
        break;
    case ST_LISTENING:
        if (now - sg_state_since > XZ_LISTEN_IDLE_TIMEOUT_MS) {
            PR_NOTICE("nothing heard for %d s, closing", XZ_LISTEN_IDLE_TIMEOUT_MS / 1000);
            __close_channel();
        }
        break;
    case ST_OFFLINE:
        if (xz_wifi_is_connected()) {
            __set_state(ST_IDLE);
        } else if (now - sg_state_since > 15000) {
            char ssid[XZ_SSID_MAX + 1] = {0};
            char passwd[XZ_PASSWD_MAX + 1] = {0};
            xz_settings_get_wifi(ssid, passwd);
            sg_state_since = now;
            if (OPRT_OK == xz_wifi_connect(ssid, passwd, XZ_WIFI_CONNECT_TIMEOUT_MS)) {
                __set_state(ST_IDLE);
            }
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- main */

static void __app_main(void)
{
    char ssid[XZ_SSID_MAX + 1] = {0};
    char passwd[XZ_PASSWD_MAX + 1] = {0};
    OPERATE_RET rt = OPRT_OK;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("==== KW002 XiaoZhi %s (TuyaOpen %s, board %s) ====", PROJECT_VERSION, OPEN_COMMIT, PLATFORM_BOARD);

    tal_sw_timer_init();
    tal_workq_init();
    tal_time_service_init(); /* wall clock used by time() / TLS, see xz_time.c */
    /* mbedtls threading hooks + DRBG; normally done by tuya_iot_init, without it every
     * handshake fails with MBEDTLS_ERR_THREADING_BAD_INPUT_DATA (-0x1c) */
    TUYA_CALL_ERR_LOG(tuya_tls_init());
    TUYA_CALL_ERR_LOG(tal_queue_create_init(&sg_ev_queue, sizeof(APP_EV_T), 16));
    TUYA_CALL_ERR_LOG(xz_settings_init());
#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif
    TUYA_CALL_ERR_LOG(board_register_hardware());
    xz_ui_init();
    TUYA_CALL_ERR_LOG(xz_audio_init());
    __keys_init();
    TUYA_CALL_ERR_LOG(xz_wifi_init(__wifi_link_cb));
    PR_NOTICE("device %s, uuid %s", xz_wifi_mac_str(), xz_settings_uuid());

    /* 1. provisioning */
    if (xz_settings_get_flag(XZ_FLAG_FORCE_PORTAL)) {
        xz_settings_set_flag(XZ_FLAG_FORCE_PORTAL, FALSE);
        __run_portal(FALSE); /* already announced before the reboot */
    }
    if (!xz_settings_get_wifi(ssid, passwd)) {
        PR_NOTICE("no wifi configured");
        __run_portal(TRUE);
    }

    /* 2. network, clock, check-in / activation */
    __connect_wifi();
    if (OPRT_OK != xz_time_sync()) {
        PR_WARN("time sync failed, TLS certificate checks may fail");
    }
    __check_in_and_activate();

    /* 3. ready */
    xz_audio_play_sound(XZ_SND_SUCCESS, TRUE);
    __set_state(ST_IDLE);
    PR_NOTICE("ready: press the talk key to chat");

    while (1) {
        APP_EV_T ev;
        if (OPRT_OK == tal_queue_fetch(sg_ev_queue, &ev, 100)) {
            switch (ev.type) {
            case EV_KEY_TALK:
                __on_talk_key();
                break;
            case EV_KEY_RESET_LONG:
                __enter_portal_by_key();
                break;
            case EV_WS_TEXT:
                __handle_server_json(ev.text);
                break;
            case EV_WS_CLOSED:
                PR_NOTICE("channel closed by server");
                __close_channel();
                break;
            case EV_WIFI_DOWN:
                __on_wifi_down();
                break;
            default:
                break;
            }
            tal_free(ev.text);
        }
        __periodic();
    }
    (void)rt;
}

static THREAD_HANDLE sg_app_thread = NULL;

static void __app_thread(void *arg)
{
    (void)arg;
    __app_main();
    tal_thread_delete(sg_app_thread);
    sg_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T cfg = {.stackDepth = 12 * 1024, .priority = THREAD_PRIO_1, .thrdname = "xz_app"};
    tal_thread_create_and_start(&sg_app_thread, NULL, NULL, __app_thread, NULL, &cfg);
}
