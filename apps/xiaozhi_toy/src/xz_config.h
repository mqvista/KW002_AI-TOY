/**
 * @file xz_config.h
 * @brief Compile time configuration of the KW002 XiaoZhi client.
 */

#ifndef __XZ_CONFIG_H__
#define __XZ_CONFIG_H__

/* ---- XiaoZhi server ---- */
#define XZ_OTA_HOST          "api.tenclass.net"
#define XZ_OTA_PORT          443
#define XZ_OTA_PATH          "/xiaozhi/ota/"
#define XZ_OTA_ACTIVATE_PATH "/xiaozhi/ota/activate"
#define XZ_LANGUAGE          "zh-CN"
#define XZ_BOARD_TYPE        "kw002-t5ai"
#define XZ_APP_NAME          "kw002-xiaozhi"
#define XZ_CHIP_MODEL        "bk7258"
#define XZ_PROTOCOL_VERSION  1

/* ---- conversation ---- */
/* 1: "realtime" listening - the mic keeps streaming while the reply plays (device AEC
 *    removes the speaker), so the user can interrupt by just talking.
 * 0: "auto" listening - the mic is off while the reply plays. */
#define XZ_LISTEN_REALTIME   1

/* ---- wake word (offline, see xz_wake.c) ---- */
#define XZ_WAKE_ENABLE       1
#define XZ_WAKE_WORD_TEXT    "小智同学"
/* 1: tell the server which word woke the device ("listen detect"), it answers with a greeting.
 * 0: only play the popup sound and listen, without requesting a spoken greeting (default). */
#define XZ_WAKE_SEND_DETECT  0

/* ---- audio ---- */
#define XZ_SAMPLE_RATE       16000
#define XZ_OPUS_FRAME_MS     60
#define XZ_OPUS_FRAME_SAMPLES (XZ_SAMPLE_RATE / 1000 * XZ_OPUS_FRAME_MS)
#define XZ_SPEAKER_VOLUME    100 /* default; a volume set by voice is kept in KV */

/* ---- amplifier and standby ---- */
#define XZ_PA_START_MS    20U                /* settle before writing the first PCM frame */
#define XZ_PA_TAIL_MS     200U               /* margin after the projected hardware drain */
#define XZ_IDLE_SLEEP_MS  (5U * 60U * 1000U) /* 0 disables automatic sleep */
#define XZ_SLEEP_RETRY_MS 60000U             /* preparation failures: retry at most once/minute */

/* ---- battery (levels from the original firmware's OCV table) ---- */
#define XZ_BAT_LOW_LEVEL      20  /* original firmware: "battery low, play alert" at <= 20 % */
#define XZ_BAT_VERY_LOW_LEVEL 10  /* second reminder */
#define XZ_BAT_RECOVER_LEVEL  25  /* re-arm the reminders above this (hysteresis) */

/* ---- Wi-Fi provisioning (SoftAP portal) ---- */
#define XZ_AP_SSID_PREFIX    "Xiaozhi-"
#define XZ_AP_IP             "192.168.4.1"
#define XZ_AP_MASK           "255.255.255.0"
#define XZ_AP_CHANNEL        6

/* ---- timing ---- */
#define XZ_WIFI_CONNECT_TIMEOUT_MS  30000
#define XZ_WS_HELLO_TIMEOUT_MS      10000
#define XZ_LISTEN_IDLE_TIMEOUT_MS   30000 /* close the channel when nothing is recognised */

#endif /* __XZ_CONFIG_H__ */
