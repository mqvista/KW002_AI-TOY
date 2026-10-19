/**
 * @file app_main.c
 * @brief KW002 XiaoZhi AI toy: boot flow, Wi-Fi provisioning, activation and conversation state machine.
 *
 * Keys: P33 talk  - press: start chat / interrupt the reply / end chat;
 *                   in the portal: repeat "请用手机连接热点，小智杠 5 C 9 0"
 *       P12 reset - hold 3 s: "entering Wi-Fi configuration mode", reboot into the SoftAP portal
 * Voice: "小智同学" while idle starts a chat (offline wake word, xz_wake.c)
 * MCP:   the server can read volume / battery / Wi-Fi and set the volume (xz_mcp.c)
 * LED (P8, PWM): idle = breathing (a blip every 3 s instead when the battery is low),
 *       listening = on, speaking = blinking.
 *       Charging is shown by a separate LED driven by the charger chip.
 * Charger plugged in / removed: "开始充电" / "充电器已拔出"
 * Standby: PA off without playback; 5 minutes idle on battery -> deep sleep, P33 wakes.
 * Network: "网络连接失败" (cannot join at boot), "网络已断开" / "网络已恢复" (link lost / back)
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
#include "tal_wifi.h"
#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
#include "lwip_init.h"
#endif

#include "board_com_api.h"

#include "xz_audio.h"
#include "xz_battery.h"
#include "xz_config.h"
#include "xz_mcp.h"
#include "xz_ota.h"
#include "xz_portal.h"
#include "xz_power.h"
#include "xz_settings.h"
#include "xz_time.h"
#include "xz_ui.h"
#include "xz_wake.h"
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
    EV_WAKE_WORD,
} APP_EV_E;

typedef struct {
    APP_EV_E type;
    char *text;
} APP_EV_T;

typedef enum {
    ST_BOOT,
    ST_PORTAL,
    ST_IDLE,
    ST_OPENING, /* websocket connected, waiting for the server hello */
    ST_LISTENING,
    ST_SPEAKING,
    ST_OFFLINE,
    ST_SLEEPING,
} APP_STATE_E;

static const char *sg_state_name[] = {"boot",      "portal",   "idle",    "opening",
                                      "listening", "speaking", "offline", "sleeping"};

/***********************************************************
***********************variable define**********************
***********************************************************/
static QUEUE_HANDLE sg_ev_queue = NULL;
static volatile APP_STATE_E sg_state = ST_BOOT;
static XZ_OTA_INFO_T sg_ota;
static char sg_session_id[64] = {0};
static BOOL_T sg_tts_stopped = FALSE;
static BOOL_T sg_listen_started = FALSE; /* realtime: listen start sent for this session */
static BOOL_T sg_wake_pending = FALSE;   /* channel opened by the wake word: report it after hello */
static uint8_t sg_bat_alerted = 0;       /* low battery reminders given: 1 = low, 2 = very low */
static int sg_chg_state = -1;            /* charger state last announced */
static SYS_TIME_T sg_chg_since = 0;      /* charger state differs from the announced one since */
static int sg_chg_prompt = -1;           /* XZ_SND_* waiting for the current reply to end */
static SYS_TIME_T sg_state_since = 0;
static char sg_auth[192];

/***********************************************************
***********************function define**********************
***********************************************************/
static void __post_event(APP_EV_E type, char *text)
{
    if (type == EV_KEY_TALK || type == EV_KEY_RESET_LONG || type == EV_WAKE_WORD) {
        xz_power_activity(); /* cancel sleep even before the main thread handles the event */
    }
    APP_EV_T ev = {.type = type, .text = text};
    if (OPRT_OK != tal_queue_post(sg_ev_queue, &ev, 0)) {
        tal_free(text);
    }
}

/* LED while idle: breathing, or the low battery blip (charging has its own hardware LED) */
static XZ_UI_E __idle_ui(void)
{
    XZ_BATTERY_T b;
    BOOL_T known = xz_battery_get(&b);

    if (known && b.level <= XZ_BAT_LOW_LEVEL) {
        return XZ_UI_LOW_BATTERY;
    }
    return XZ_UI_STANDBY;
}

static void __set_state(APP_STATE_E st)
{
    if (st == sg_state) {
        return;
    }
    PR_NOTICE("state %s -> %s", sg_state_name[sg_state], sg_state_name[st]);
    sg_state = st;
    sg_state_since = tal_system_get_millisecond();
    if (st == ST_IDLE || st == ST_OFFLINE) {
        xz_power_activity();
    }
#if XZ_WAKE_ENABLE
    if (st != ST_IDLE) {
        xz_wake_enable(FALSE); /* in a conversation anything said goes to the server anyway */
    }
#endif

    switch (st) {
    case ST_SLEEPING:
        xz_ui_set(XZ_UI_OFF);
        break;
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
        xz_ui_set(__idle_ui());
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
    if (TDL_BUTTON_PRESS_DOWN == event) {
        xz_power_activity();
    } else if (TDL_BUTTON_LONG_PRESS_START == event) {
        __post_event(EV_KEY_RESET_LONG, NULL);
    }
}

static void __wake_cb(const char *word)
{
    (void)word;
    __post_event(EV_WAKE_WORD, NULL);
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
        tdl_button_event_register(reset, TDL_BUTTON_PRESS_DOWN, __reset_key_cb);
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

/* "请用手机连接热点，小智杠" + the SSID suffix spelled out, e.g. "五 C 九 零" */
static void __announce_ap(void)
{
    const char *ssid = xz_portal_ssid();

    PR_NOTICE("announce: connect to \"%s\"", ssid);
    if (strlen(ssid) <= strlen(XZ_AP_SSID_PREFIX)) {
        return;
    }
    /* wait in between: 20 ms prompt frames, the whole phrase would overflow the play queue */
    xz_audio_play_sound(XZ_SND_CONNECT_AP, TRUE);
    for (const char *c = ssid + strlen(XZ_AP_SSID_PREFIX); *c; c++) {
        int v = -1;
        if (*c >= '0' && *c <= '9') {
            v = *c - '0';
        } else if (*c >= 'A' && *c <= 'F') {
            v = *c - 'A' + 10;
        } else if (*c >= 'a' && *c <= 'f') {
            v = *c - 'a' + 10;
        }
        if (v >= 0) {
            xz_audio_play_sound((XZ_SOUND_E)(XZ_SND_SSID_0 + v), FALSE);
        }
    }
    xz_audio_wait_idle(15000);
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
    __announce_ap(); /* the AP is up: tell the user which network to join */

    /* the portal reboots the device once credentials are saved */
    while (1) {
        APP_EV_T ev;
        if (OPRT_OK == tal_queue_fetch(sg_ev_queue, &ev, 1000)) {
            tal_free(ev.text);
            if (EV_KEY_TALK == ev.type) {
                __announce_ap(); /* say it again */
            }
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
    if (!up && sg_state != ST_BOOT && sg_state != ST_PORTAL && sg_state != ST_SLEEPING) {
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
            xz_audio_play_sound(XZ_SND_NET_FAILED, TRUE);
            xz_settings_set_flag(XZ_FLAG_FORCE_PORTAL, TRUE);
            tal_system_reset();
        }
        xz_ui_set(XZ_UI_ERROR);
        xz_audio_play_sound(XZ_SND_NET_FAILED, TRUE);
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

static void __mcp_send(const char *payload)
{
    size_t n = strlen(payload) + 96;
    char *buf = tal_malloc(n);

    if (NULL == buf) {
        return;
    }
    snprintf(buf, n, "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":%s}", sg_session_id, payload);
    PR_DEBUG(">> %.160s", buf);
    xz_ws_send_text(buf);
    tal_free(buf);
}

static void __start_listening(void)
{
    sg_tts_stopped = FALSE;
#if XZ_LISTEN_REALTIME
    /* realtime: one listen start per session, the mic then streams continuously */
    if (!sg_listen_started) {
        __send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"realtime\"}",
                    sg_session_id);
        sg_listen_started = TRUE;
    }
#else
    __send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}", sg_session_id);
#endif
    xz_audio_capture(TRUE, __mic_send_cb);
    __set_state(ST_LISTENING);
}

static void __close_channel(void)
{
    xz_audio_capture(FALSE, NULL);
    xz_ws_close();
    sg_session_id[0] = '\0';
    sg_listen_started = FALSE;
    sg_wake_pending = FALSE;
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
        sg_wake_pending = FALSE;
        __set_state(ST_IDLE);
        return;
    }
    __send_json("{\"type\":\"hello\",\"version\":%d,\"features\":{\"mcp\":true},\"transport\":\"websocket\","
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
#if XZ_WAKE_SEND_DETECT
            if (sg_wake_pending) {
                /* like xiaozhi-esp32: the server answers the wake word with a greeting */
                __send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"%s\"}",
                            sg_session_id, XZ_WAKE_WORD_TEXT);
            }
#endif
            sg_wake_pending = FALSE;
            __start_listening();
        }
    } else if (strcmp(t, "stt") == 0) {
        cJSON *txt = cJSON_GetObjectItem(root, "text");
        PR_NOTICE("[user] %s", cJSON_IsString(txt) ? txt->valuestring : "");
        if (sg_state == ST_SPEAKING) {
            /* realtime barge-in: the user talked over the reply, drop what is still
             * buffered of the old answer; the new answer arrives with its own tts start */
            PR_NOTICE("barge-in, old reply dropped");
            xz_audio_stop_playback();
            __set_state(ST_LISTENING);
        }
        sg_state_since = tal_system_get_millisecond();
    } else if (strcmp(t, "mcp") == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload)) {
            xz_mcp_handle(payload, __mcp_send);
        }
    } else if (strcmp(t, "llm") == 0) {
        cJSON *emo = cJSON_GetObjectItem(root, "emotion");
        PR_DEBUG("[emotion] %s", cJSON_IsString(emo) ? emo->valuestring : "");
    } else if (strcmp(t, "tts") == 0) {
        cJSON *st = cJSON_GetObjectItem(root, "state");
        const char *s = cJSON_IsString(st) ? st->valuestring : "";
        if (strcmp(s, "start") == 0) {
#if !XZ_LISTEN_REALTIME
            xz_audio_capture(FALSE, NULL); /* auto mode: mic off while the reply plays */
#endif
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

static void __on_wake_word(void)
{
    if (sg_state != ST_IDLE) {
        return; /* only armed while idle, a late event may still arrive */
    }
    PR_NOTICE("wake word \"%s\"", XZ_WAKE_WORD_TEXT);
    xz_audio_play_sound(XZ_SND_POPUP, FALSE); /* instant feedback, the connection takes ~1 s */
    sg_wake_pending = TRUE;
    __open_channel();
}

static void __on_wifi_down(void)
{
    if (ST_OFFLINE == sg_state) {
        return; /* failed reconnect attempts report "disconnected" again */
    }
    PR_WARN("wifi link down");
    xz_audio_capture(FALSE, NULL);
    xz_ws_close();
    sg_session_id[0] = '\0';
    sg_listen_started = FALSE;
    sg_wake_pending = FALSE;
    __set_state(ST_OFFLINE);
    xz_audio_stop_playback(); /* the rest of a reply will not come */
    xz_audio_play_sound(XZ_SND_NET_LOST, FALSE);
}

static void __on_wifi_back(void)
{
    PR_NOTICE("wifi link back");
    __set_state(ST_IDLE);
    xz_audio_play_sound(XZ_SND_NET_RESTORED, FALSE);
}

/*
 * "开始充电" / "充电器已拔出". CHRG bounces for a moment when the cable goes in, so a new
 * state is announced once it has been stable for CHG_SETTLE_MS; the prompt is held back
 * while a reply is being spoken. Polls the cached state (updated by the power driver's
 * charger thread), so nothing is lost while the boot flow drops events.
 */
#define CHG_SETTLE_MS 800

static void __charger_prompt(void)
{
    SYS_TIME_T now = tal_system_get_millisecond();
    XZ_BATTERY_T b;
    BOOL_T known = xz_battery_get(&b);

    if ((int)b.charging == sg_chg_state) {
        sg_chg_since = 0;
    } else if (0 == sg_chg_since) {
        sg_chg_since = now;
    } else if (now - sg_chg_since >= CHG_SETTLE_MS) {
        sg_chg_since = 0;
        sg_chg_state = (int)b.charging;
        PR_NOTICE("charger %s, battery %u pct (%u mV)%s", b.charging ? "connected" : "disconnected", b.level, b.mv,
                  known ? "" : " not measured yet");
        if (b.charging) {
            sg_bat_alerted = 0;
        }
        sg_chg_prompt = b.charging ? XZ_SND_CHARGING : XZ_SND_CHARGER_REMOVED;
    }
    if (sg_chg_prompt >= 0 && sg_state != ST_SPEAKING) {
        xz_audio_play_sound((XZ_SOUND_E)sg_chg_prompt, FALSE);
        sg_chg_prompt = -1;
    }
}

/* Low battery reminders: once at <= 20 %, again at <= 10 %, only while idle and quiet. */
static void __battery_check(void)
{
    XZ_BATTERY_T b;
    uint8_t stage;

    if (!xz_battery_get(&b)) {
        return;
    }
    if (b.charging || b.level >= XZ_BAT_RECOVER_LEVEL) {
        sg_bat_alerted = 0;
        return;
    }
    stage = b.level <= XZ_BAT_VERY_LOW_LEVEL ? 2 : (b.level <= XZ_BAT_LOW_LEVEL ? 1 : 0);
    if (stage > sg_bat_alerted && sg_state == ST_IDLE && !xz_audio_is_playing()) {
        PR_WARN("battery low: %u pct (%u mV)", b.level, b.mv);
        sg_bat_alerted = stage;
        xz_audio_play_sound(XZ_SND_LOW_BATTERY, FALSE);
    }
}

static BOOL_T __try_idle_sleep(void)
{
    if (!xz_power_should_sleep(sg_state == ST_IDLE || sg_state == ST_OFFLINE)) {
        return FALSE;
    }
    __set_state(ST_SLEEPING); /* disarms KWS and ignores the intentional Wi-Fi shutdown */
    xz_audio_capture(FALSE, NULL);
    xz_ws_close();
    OPERATE_RET rt = xz_audio_suspend();
    if (OPRT_OK == rt) {
        rt = tal_wifi_set_work_mode(WWM_POWERDOWN);
    }
    if (OPRT_OK == rt) {
        rt = xz_power_enter_sleep(); /* no return on success */
    }
    PR_WARN("sleep cancelled or failed (%d), restoring standby", rt);
    xz_power_defer();
    if (OPRT_OK != xz_audio_resume()) {
        PR_ERR("audio restore failed; rebooting");
        tal_system_reset();
        return TRUE;
    }
    /* If Wi-Fi was powered down, ST_OFFLINE owns reconnection. Queued key
     * events are left intact and will be handled on the next main-loop turn. */
    __set_state(xz_wifi_is_connected() ? ST_IDLE : ST_OFFLINE);
    return TRUE;
}

static void __periodic(void)
{
    SYS_TIME_T now = tal_system_get_millisecond();

    xz_battery_poll(xz_audio_is_playing());
    __charger_prompt();
    __battery_check();
    if (__try_idle_sleep()) {
        return;
    }

    switch (sg_state) {
    case ST_IDLE:
#if XZ_WAKE_ENABLE
        /* armed once the speaker is quiet, e.g. after the goodbye of a closed session */
        xz_wake_enable(!xz_audio_is_playing());
#endif
        xz_ui_set(__idle_ui());
        break;
    case ST_OPENING:
        if (now - sg_state_since > XZ_WS_HELLO_TIMEOUT_MS) {
            PR_ERR("no server hello");
            xz_audio_play_sound(XZ_SND_EXCLAMATION, FALSE);
            __close_channel();
        }
        break;
    case ST_SPEAKING:
        if (sg_tts_stopped && !xz_audio_is_playing()) {
            __start_listening(); /* keep the conversation going */
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
            __on_wifi_back();
        } else if (now - sg_state_since > 15000) {
            char ssid[XZ_SSID_MAX + 1] = {0};
            char passwd[XZ_PASSWD_MAX + 1] = {0};
            xz_settings_get_wifi(ssid, passwd);
            sg_state_since = now;
            if (OPRT_OK == xz_wifi_connect(ssid, passwd, XZ_WIFI_CONNECT_TIMEOUT_MS)) {
                __on_wifi_back();
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
    if (OPRT_OK != board_audio_manual_pa_enable()) {
        PR_ERR("cannot initialize speaker power control");
        return;
    }
    TUYA_CALL_ERR_LOG(board_register_hardware());
    xz_ui_init();
    if (OPRT_OK != xz_audio_init()) {
        PR_ERR("audio initialization failed; amplifier remains off");
        return;
    }
    xz_audio_set_volume((uint8_t)xz_settings_get_int(XZ_KEY_VOLUME, XZ_SPEAKER_VOLUME));
    if (OPRT_OK == xz_battery_init(NULL)) {
        XZ_BATTERY_T b;
        xz_battery_get(&b);
        sg_chg_state = (int)b.charging; /* only changes are announced, not the state at boot */
    }
#if XZ_WAKE_ENABLE
    if (OPRT_OK == xz_wake_init(__wake_cb)) {
        xz_audio_set_mic_tap(xz_wake_feed);
    }
#endif
    __keys_init();
    TUYA_CALL_ERR_LOG(xz_power_init());
    TUYA_CALL_ERR_LOG(xz_wifi_init(__wifi_link_cb));
    PR_NOTICE("device %s, uuid %s, volume %u", xz_wifi_mac_str(), xz_settings_uuid(), xz_audio_get_volume());

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
    PR_NOTICE("ready: say \"%s\" or press the talk key to chat", XZ_WAKE_WORD_TEXT);

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
            case EV_WAKE_WORD:
                __on_wake_word();
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
