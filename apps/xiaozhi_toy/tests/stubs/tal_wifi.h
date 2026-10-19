#ifndef TEST_TAL_WIFI_H
#define TEST_TAL_WIFI_H
#include "test_sdk.h"

typedef enum {
    WSS_IDLE, WSS_CONNECTING, WSS_PASSWD_WRONG, WSS_NO_AP_FOUND,
    WSS_CONN_FAIL, WSS_CONN_SUCCESS, WSS_GOT_IP, WSS_DHCP_FAIL
} WF_STATION_STAT_E;
#define WF_STATION 0
typedef struct {
    char nwipstr[16];
    char nwgwstr[16];
} NW_IP_S;

OPERATE_RET tal_wifi_station_get_status(WF_STATION_STAT_E *status);
OPERATE_RET tal_wifi_station_get_conn_ap_rssi(int8_t *rssi);
OPERATE_RET tal_wifi_get_ip(int wf, NW_IP_S *ip);
#endif
