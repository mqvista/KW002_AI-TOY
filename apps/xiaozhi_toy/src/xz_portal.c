/**
 * @file xz_portal.c
 * @brief SoftAP Wi-Fi provisioning portal (open AP + captive web page).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tal_api.h"
#include "tal_wifi.h"
#include "http_host.h"
#include "http_site.h"
#include "http_captive.h"

#include "xz_config.h"
#include "xz_dns.h"
#include "xz_portal.h"
#include "xz_settings.h"
#include "xz_wifi.h"

/***********************************************************
************************macro define************************
***********************************************************/
#define PORTAL_MAX_AP        20
#define PORTAL_PAGE_SIZE     (8 * 1024)
#define PORTAL_BODY_MAX      512
#define PORTAL_REBOOT_DELAY  2500

/***********************************************************
***********************variable define**********************
***********************************************************/
typedef struct {
    char ssid[XZ_SSID_MAX + 1];
    int8_t rssi;
    BOOL_T secure;
} PORTAL_AP_T;

static PORTAL_AP_T sg_aps[PORTAL_MAX_AP];
static uint32_t sg_ap_cnt = 0;
static char sg_ap_ssid[WIFI_SSID_LEN + 1] = {0};
static HTTP_HOST_HANDLE_T sg_host = NULL;
static HTTP_SITE_HANDLE_T sg_site = NULL;
static volatile SYS_TIME_T sg_reboot_at = 0;

static const char sg_page_head[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>小智 AI 配网</title><style>"
    "body{font-family:sans-serif;background:#f2f4f7;margin:0;padding:16px;color:#222}"
    ".c{max-width:420px;margin:0 auto;background:#fff;border-radius:12px;padding:20px;"
    "box-shadow:0 2px 8px rgba(0,0,0,.08)}"
    "h2{margin:0 0 4px}.sub{color:#888;font-size:13px;margin:0 0 16px}"
    "label{display:block;margin:12px 0 6px;font-size:14px;color:#555}"
    "input{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border:1px solid #ccc;border-radius:8px}"
    ".l{max-height:220px;overflow-y:auto;margin-top:8px;border:1px solid #eee;border-radius:8px}"
    ".l a{display:flex;justify-content:space-between;padding:10px;border-bottom:1px solid #f0f0f0;"
    "color:#222;text-decoration:none}.l a:last-child{border-bottom:0}.l span{color:#999;font-size:12px}"
    "button{width:100%;margin-top:18px;padding:12px;font-size:16px;border:0;border-radius:8px;"
    "background:#1677ff;color:#fff}.t{color:#999;font-size:12px;margin-top:12px}"
    "</style></head><body><div class=\"c\">";

static const char sg_page_tail[] =
    "</div><script>function p(e){document.getElementById('s').value=e.getAttribute('data-s');"
    "document.getElementById('w').focus();return false}</script></body></html>";

static const char sg_page_saved[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>小智 AI 配网</title>"
    "<style>body{font-family:sans-serif;background:#f2f4f7;padding:16px}"
    ".c{max-width:420px;margin:0 auto;background:#fff;border-radius:12px;padding:20px}</style></head>"
    "<body><div class=\"c\"><h2>已保存</h2><p>设备正在重启并连接 Wi-Fi，热点会自动关闭。</p>"
    "<p>如果连接失败，设备会重新进入配网模式。</p></div></body></html>";

/***********************************************************
***********************function define**********************
***********************************************************/
/* Append src to the page buffer with HTML escaping. */
static void __html_escape_cat(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(dst);

    for (; *src && n + 7 < dst_size; src++) {
        const char *rep = NULL;
        switch (*src) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: break;
        }
        if (rep) {
            strcpy(dst + n, rep);
            n += strlen(rep);
        } else {
            dst[n++] = *src;
            dst[n] = '\0';
        }
    }
}

static void __url_decode(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static BOOL_T __form_field(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;

    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            size_t len = e ? (size_t)(e - v) : strlen(v);
            if (len >= out_len) {
                len = out_len - 1;
            }
            memcpy(out, v, len);
            out[len] = '\0';
            __url_decode(out);
            return TRUE;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return FALSE;
}

static void __scan_networks(void)
{
    AP_IF_S *aps = NULL;
    uint32_t num = 0;

    sg_ap_cnt = 0;
    if (OPRT_OK != tal_wifi_set_work_mode(WWM_STATION) || OPRT_OK != tal_wifi_all_ap_scan(&aps, &num) || !aps) {
        PR_ERR("wifi scan failed");
        return;
    }

    for (uint32_t i = 0; i < num; i++) {
        char ssid[XZ_SSID_MAX + 1] = {0};
        uint32_t len = aps[i].s_len > XZ_SSID_MAX ? XZ_SSID_MAX : aps[i].s_len;
        uint32_t j;

        memcpy(ssid, aps[i].ssid, len);
        if (ssid[0] == '\0') {
            continue; /* hidden network */
        }
        for (j = 0; j < sg_ap_cnt; j++) { /* keep the strongest of duplicate SSIDs */
            if (strcmp(sg_aps[j].ssid, ssid) == 0) {
                break;
            }
        }
        if (j < sg_ap_cnt) {
            if (aps[i].rssi > sg_aps[j].rssi) {
                sg_aps[j].rssi = aps[i].rssi;
            }
            continue;
        }
        if (sg_ap_cnt >= PORTAL_MAX_AP) {
            /* replace the weakest entry */
            uint32_t w = 0;
            for (j = 1; j < sg_ap_cnt; j++) {
                if (sg_aps[j].rssi < sg_aps[w].rssi) {
                    w = j;
                }
            }
            if (aps[i].rssi <= sg_aps[w].rssi) {
                continue;
            }
            j = w;
        } else {
            j = sg_ap_cnt++;
        }
        strcpy(sg_aps[j].ssid, ssid);
        sg_aps[j].rssi = aps[i].rssi;
        sg_aps[j].secure = aps[i].security != 0;
    }
    tal_wifi_release_ap(aps);

    /* sort by signal strength */
    for (uint32_t a = 0; a + 1 < sg_ap_cnt; a++) {
        for (uint32_t b = a + 1; b < sg_ap_cnt; b++) {
            if (sg_aps[b].rssi > sg_aps[a].rssi) {
                PORTAL_AP_T t = sg_aps[a];
                sg_aps[a] = sg_aps[b];
                sg_aps[b] = t;
            }
        }
    }
    PR_NOTICE("wifi scan: %u networks", sg_ap_cnt);
}

static OPERATE_RET __page_index_cb(const HTTP_HOST_REQUEST_T *req, void *user_ctx)
{
    OPERATE_RET rt = OPRT_OK;
    char *page = tal_malloc(PORTAL_PAGE_SIZE);
    char line[96];

    (void)user_ctx;
    if (NULL == page) {
        return http_host_reply(req->client_fd, "500 Internal Server Error", "text/plain", "no memory");
    }

    snprintf(page, PORTAL_PAGE_SIZE, "%s<h2>小智 AI 配网</h2><p class=\"sub\">设备 %s</p>"
             "<form method=\"POST\" action=\"/save\"><label>Wi-Fi 名称</label>"
             "<input id=\"s\" name=\"ssid\" maxlength=\"32\" required autocomplete=\"off\">",
             sg_page_head, xz_wifi_mac_str());

    if (sg_ap_cnt > 0) {
        strcat(page, "<div class=\"l\">");
        for (uint32_t i = 0; i < sg_ap_cnt; i++) {
            strcat(page, "<a href=\"#\" onclick=\"return p(this)\" data-s=\"");
            __html_escape_cat(page, PORTAL_PAGE_SIZE, sg_aps[i].ssid);
            strcat(page, "\">");
            __html_escape_cat(page, PORTAL_PAGE_SIZE, sg_aps[i].ssid);
            snprintf(line, sizeof(line), "<span>%s %ddBm</span></a>", sg_aps[i].secure ? "&#128274;" : "",
                     sg_aps[i].rssi);
            strcat(page, line);
        }
        strcat(page, "</div>");
    }

    strcat(page, "<label>密码</label><input id=\"w\" name=\"password\" type=\"password\" maxlength=\"64\">"
                 "<button type=\"submit\">保存并连接</button></form>"
                 "<p class=\"t\">仅支持 2.4GHz Wi-Fi。保存后设备会重启并连接网络。</p>");
    strcat(page, sg_page_tail);

    rt = http_host_reply(req->client_fd, "200 OK", "text/html; charset=utf-8", page);
    PR_NOTICE("portal: page %u bytes, reply %d", (unsigned)strlen(page), rt);
    tal_free(page);
    return rt;
}

static OPERATE_RET __page_save_cb(const HTTP_HOST_REQUEST_T *req, void *user_ctx)
{
    char body[PORTAL_BODY_MAX + 1] = {0};
    char ssid[XZ_SSID_MAX + 1] = {0};
    char passwd[XZ_PASSWD_MAX + 1] = {0};
    uint32_t len;

    (void)user_ctx;
    if (NULL == req->body || 0 == req->body_len) {
        return http_host_reply(req->client_fd, "400 Bad Request", "text/plain; charset=utf-8", "empty request");
    }
    len = req->body_len > PORTAL_BODY_MAX ? PORTAL_BODY_MAX : req->body_len;
    memcpy(body, req->body, len);

    __form_field(body, "ssid", ssid, sizeof(ssid));
    __form_field(body, "password", passwd, sizeof(passwd));
    if (ssid[0] == '\0') {
        return http_host_reply(req->client_fd, "400 Bad Request", "text/plain; charset=utf-8", "请输入 Wi-Fi 名称");
    }

    PR_NOTICE("portal: save wifi \"%s\" (password %u chars)", ssid, (unsigned)strlen(passwd));
    xz_settings_set_wifi(ssid, passwd);
    xz_settings_set_flag(XZ_FLAG_WIFI_UNTESTED, TRUE);
    xz_settings_set_flag(XZ_FLAG_FORCE_PORTAL, FALSE);
    sg_reboot_at = tal_system_get_millisecond() + PORTAL_REBOOT_DELAY;

    return http_host_reply(req->client_fd, "200 OK", "text/html; charset=utf-8", sg_page_saved);
}

/*
 * http_host closes the socket right after the handler returns. On this lwIP that close
 * turns into a RST while the response is still in flight and the browser gets
 * "connection reset" with no data. The response carries "Connection: close", so wait
 * here until the client has read it and closed its side (recv timeout is the host's
 * 30 ms slice, ~1.5 s max). Note: shutdown(SHUT_WR) + close crashes this lwIP
 * (NULL pcb in lwip_netconn_do_close_internal), so it is not used.
 */
static void __graceful_finish(int fd)
{
    char tmp[128];

    for (int i = 0; i < 50; i++) {
        int n = tal_net_recv(fd, tmp, sizeof(tmp));
        if (n == 0 || (n < 0 && n != OPRT_TIMEOUT)) {
            break; /* client closed */
        }
    }
}

static OPERATE_RET __http_dispatch(const HTTP_HOST_REQUEST_T *req)
{
    OPERATE_RET rt;

    if (OPRT_OK == http_captive_handle_http(req)) {
        return OPRT_OK; /* OS connectivity probe answered with a redirect */
    }
    rt = http_site_dispatch(sg_site, req);
    if (OPRT_NOT_FOUND == rt) {
        if (strcmp(req->method, "GET") == 0) {
            return http_captive_redirect_to_portal(req);
        }
        return http_host_reply_not_found(req->client_fd);
    }
    return rt;
}

static OPERATE_RET __http_request_cb(const HTTP_HOST_REQUEST_T *req, void *user_ctx)
{
    OPERATE_RET rt;
    char host[64] = {0};

    (void)user_ctx;
    if (NULL == req || NULL == req->raw) {
        return OPRT_INVALID_PARM;
    }
    http_host_get_header_value(req->raw, req->raw_len, "Host", host, sizeof(host));
    PR_NOTICE("portal: %s %s (Host: %s)", req->method, req->path, host);

    rt = __http_dispatch(req);
    __graceful_finish(req->client_fd);
    return rt;
}

static void __http_idle_cb(HTTP_HOST_HANDLE_T host, void *user_ctx)
{
    (void)host;
    (void)user_ctx;
    if (sg_reboot_at && tal_system_get_millisecond() >= sg_reboot_at) {
        PR_NOTICE("portal: credentials saved, rebooting");
        tal_system_sleep(100);
        tal_system_reset();
    }
}

OPERATE_RET xz_portal_run(void)
{
    OPERATE_RET rt = OPRT_OK;
    WF_AP_CFG_IF_S ap = {0};
    HTTP_HOST_CFG_T host_cfg = {0};
    const char *mac = xz_wifi_mac_str();
    size_t mac_len = strlen(mac);

    __scan_networks();

    /* "Xiaozhi-" + last two MAC bytes, e.g. Xiaozhi-5C90 */
    snprintf(sg_ap_ssid, sizeof(sg_ap_ssid), "%s%c%c%c%c", XZ_AP_SSID_PREFIX,
             mac_len >= 17 ? mac[12] : '0', mac_len >= 17 ? mac[13] : '0',
             mac_len >= 17 ? mac[15] : '0', mac_len >= 17 ? mac[16] : '0');
    for (char *c = sg_ap_ssid + strlen(XZ_AP_SSID_PREFIX); *c; c++) { /* upper-case the MAC part only */
        if (*c >= 'a' && *c <= 'f') {
            *c -= 'a' - 'A';
        }
    }

    ap.s_len = (uint8_t)strlen(sg_ap_ssid);
    memcpy(ap.ssid, sg_ap_ssid, ap.s_len);
    ap.p_len = 0;
    ap.md = WAAM_OPEN;
    ap.chan = XZ_AP_CHANNEL;
    ap.max_conn = 4;
    ap.ms_interval = 100;
    snprintf(ap.ip.nwipstr, sizeof(ap.ip.nwipstr), "%s", XZ_AP_IP);
    snprintf(ap.ip.nwmaskstr, sizeof(ap.ip.nwmaskstr), "%s", XZ_AP_MASK);
    snprintf(ap.ip.nwgwstr, sizeof(ap.ip.nwgwstr), "%s", XZ_AP_IP);

    TUYA_CALL_ERR_RETURN(tal_wifi_set_work_mode(WWM_SOFTAP));
    TUYA_CALL_ERR_RETURN(tal_wifi_ap_start(&ap));
    PR_NOTICE("portal: softap \"%s\" at http://%s/", sg_ap_ssid, XZ_AP_IP);

    TUYA_CALL_ERR_RETURN(http_captive_start(&(HTTP_CAPTIVE_CFG_T){
        .ap_ip = XZ_AP_IP,
        .redirect_unmatched_get = TRUE,
    }));
    TUYA_CALL_ERR_RETURN(http_site_create(&sg_site, HTTP_SITE_DEFAULT_MAX_ROUTES));
    TUYA_CALL_ERR_RETURN(http_site_add_handler(sg_site, "GET", "/", __page_index_cb, NULL));
    TUYA_CALL_ERR_RETURN(http_site_add_handler(sg_site, "POST", "/save", __page_save_cb, NULL));

    host_cfg.port = 80;
    host_cfg.backlog = 4;
    host_cfg.listen_recv_timeout_ms = 2500;
    host_cfg.listen_send_timeout_ms = 2500;
    host_cfg.client_recv_slice_ms = 30;
    host_cfg.client_send_timeout_ms = 2500;
    host_cfg.client_wait_max_rounds = 120;
    host_cfg.max_request_size = 2048;
    host_cfg.stack_depth = 8 * 1024;
    host_cfg.priority = THREAD_PRIO_2;
    host_cfg.thread_name = "xz_portal";
    TUYA_CALL_ERR_RETURN(http_host_start(&sg_host, &host_cfg, __http_request_cb, __http_idle_cb, NULL));

    TUYA_CALL_ERR_LOG(xz_dns_start(XZ_AP_IP));
    return rt;
}
