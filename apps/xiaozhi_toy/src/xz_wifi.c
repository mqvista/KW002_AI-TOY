/**
 * @file xz_wifi.c
 * @brief Wi-Fi station helpers.
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_wifi.h"

#include "xz_wifi.h"

static XZ_WIFI_LINK_CB sg_link_cb = NULL;
static char sg_mac_str[18] = {0};

static void __wifi_event_cb(WF_EVENT_E event, void *arg)
{
    (void)arg;
    PR_DEBUG("wifi event %d", event);
    if (NULL == sg_link_cb) {
        return;
    }
    if (WFE_CONNECTED == event) {
        sg_link_cb(TRUE);
    } else if (WFE_DISCONNECTED == event) {
        sg_link_cb(FALSE);
    }
}

OPERATE_RET xz_wifi_init(XZ_WIFI_LINK_CB cb)
{
    OPERATE_RET rt = OPRT_OK;
    NW_MAC_S mac = {0};

    sg_link_cb = cb;
    TUYA_CALL_ERR_RETURN(tal_wifi_init(__wifi_event_cb));

    if (OPRT_OK == tal_wifi_get_mac(WF_STATION, &mac)) {
        snprintf(sg_mac_str, sizeof(sg_mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac.mac[0], mac.mac[1], mac.mac[2],
                 mac.mac[3], mac.mac[4], mac.mac[5]);
    }
    return rt;
}

OPERATE_RET xz_wifi_connect(const char *ssid, const char *passwd, uint32_t timeout_ms)
{
    OPERATE_RET rt = OPRT_OK;
    WF_STATION_STAT_E stat = WSS_IDLE;
    uint32_t waited = 0;

    TUYA_CALL_ERR_RETURN(tal_wifi_set_work_mode(WWM_STATION));
    PR_NOTICE("connecting to wifi \"%s\"", ssid);
    TUYA_CALL_ERR_RETURN(tal_wifi_station_connect((int8_t *)ssid, (int8_t *)passwd));

    while (waited < timeout_ms) {
        tal_system_sleep(200);
        waited += 200;
        if (OPRT_OK == tal_wifi_station_get_status(&stat) && WSS_GOT_IP == stat) {
            NW_IP_S ip = {0};
            tal_wifi_get_ip(WF_STATION, &ip);
            PR_NOTICE("wifi connected, ip %s, %u ms", ip.nwipstr, waited);
            return OPRT_OK;
        }
    }

    PR_ERR("wifi connect timeout, last status %d", stat);
    return OPRT_TIMEOUT;
}

BOOL_T xz_wifi_is_connected(void)
{
    WF_STATION_STAT_E stat = WSS_IDLE;
    return OPRT_OK == tal_wifi_station_get_status(&stat) && WSS_GOT_IP == stat;
}

const char *xz_wifi_mac_str(void)
{
    return sg_mac_str;
}
