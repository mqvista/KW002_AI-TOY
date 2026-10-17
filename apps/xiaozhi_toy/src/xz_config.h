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

/* ---- audio ---- */
#define XZ_SAMPLE_RATE       16000
#define XZ_OPUS_FRAME_MS     60
#define XZ_OPUS_FRAME_SAMPLES (XZ_SAMPLE_RATE / 1000 * XZ_OPUS_FRAME_MS)
#define XZ_SPEAKER_VOLUME    100

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
