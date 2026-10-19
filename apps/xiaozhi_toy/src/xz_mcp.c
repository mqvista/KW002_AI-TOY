/**
 * @file xz_mcp.c
 * @brief Device side MCP server, compatible with xiaozhi-esp32 (main/mcp_server.cc).
 *
 * The server sends {"type":"mcp","payload":<JSON-RPC 2.0>} after the hello when the device
 * announced "features":{"mcp":true}: initialize, tools/list, then tools/call whenever the
 * LLM decides to use a tool. Tool names and schemas follow xiaozhi-esp32 so the official
 * server prompts work unchanged:
 *   self.get_device_status         volume, battery level / charging, Wi-Fi
 *   self.network.get_status        live Wi-Fi / IP / signal / XiaoZhi connection
 *   self.network.get_public_ip     on-demand external IPv4 lookup
 *   self.audio_speaker.set_volume  0..100, kept in KV
 */

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_wifi.h"

#include "xz_audio.h"
#include "xz_battery.h"
#include "xz_config.h"
#include "xz_mcp.h"
#include "xz_settings.h"
#include "xz_public_ip.h"
#include "xz_ws.h"

static const char sg_tools_json[] =
    "{\"tools\":["
    "{\"name\":\"self.get_device_status\","
    "\"description\":\"Provides the real-time information of the device, including the current status of the "
    "audio speaker, battery, network, etc.\\nUse this tool for: \\n1. Answering questions about current "
    "condition (e.g. what is the current volume of the audio speaker? how much battery is left? is it "
    "charging? is Wi-Fi connected?)\\n2. As the first step to control the device (e.g. turn up / down the volume of the audio "
    "speaker, etc.)\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"self.network.get_status\","
    "\"description\":\"Query this device's current network connection: Wi-Fi connection state, SSID, IPv4 "
    "address, gateway, signal strength in dBm and XiaoZhi service connection. Use when the user asks "
    "whether the device is online, which Wi-Fi it uses, its local IP address, or how good the Wi-Fi signal is. "
    "For public, external or WAN IP addresses use self.network.get_public_ip instead. "
    "A Wi-Fi connection alone does not prove Internet access. internet_status=xiaozhi_reachable means "
    "the XiaoZhi WebSocket is currently connected; unverified means Internet access has not been tested. "
    "Unknown or null values must not be reported as a successful connection or strong signal.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"self.network.get_public_ip\","
    "\"description\":\"Query the current public IPv4 address of this device's Internet connection using an "
    "external HTTPS service. Use when the user asks for public IP, external IP, WAN IP or Internet egress "
    "address. The result is the network's NAT/VPN/ISP egress address, which can differ from the router WAN "
    "address. It is not the local Wi-Fi IP. Requires Internet access. On failure, say lookup failed; "
    "never substitute a local or previously known IP address.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"self.audio_speaker.set_volume\","
    "\"description\":\"Set the volume of the audio speaker. If the current volume is unknown, you must call "
    "`self.get_device_status` tool first and then call this tool.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"volume\":{\"type\":\"integer\",\"minimum\":0,"
    "\"maximum\":100}},\"required\":[\"volume\"]}}"
    "]}";

static void __reply(int id, const char *key, const char *value, XZ_MCP_SEND_CB send)
{
    size_t n = strlen(value) + 64;
    char *buf = tal_malloc(n);

    if (NULL == buf) {
        return;
    }
    snprintf(buf, n, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"%s\":%s}", id, key, value);
    send(buf);
    tal_free(buf);
}

static void __reply_error(int id, int code, const char *msg, XZ_MCP_SEND_CB send)
{
    char err[160];

    PR_WARN("mcp error %d: %s", code, msg);
    snprintf(err, sizeof(err), "{\"code\":%d,\"message\":\"%s\"}", code, msg);
    __reply(id, "error", err, send);
}

/* Tool result: {"content":[{"type":"text","text":...}],"isError":false} */
static void __reply_tool_text(int id, const char *text, BOOL_T error, XZ_MCP_SEND_CB send)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = cJSON_CreateObject();
    char *str = NULL;

    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(result, "isError", error);
    str = cJSON_PrintUnformatted(result);
    if (str) {
        __reply(id, "result", str, send);
        cJSON_free(str);
    }
    cJSON_Delete(result);
}

static void __reply_text(int id, const char *text, XZ_MCP_SEND_CB send)
{
    __reply_tool_text(id, text, FALSE, send);
}

static void __tool_public_ip(int id, XZ_MCP_SEND_CB send)
{
    char ip[16] = {0};
    OPERATE_RET rt = xz_public_ip_query(ip);
    if (rt != OPRT_OK) {
        __reply_tool_text(id, "Public IPv4 lookup failed: network unavailable, timeout, TLS, service or "
                          "invalid response. Do not substitute the local IP or an earlier result.", TRUE, send);
        return;
    }
    char result[112];
    snprintf(result, sizeof(result), "{\"public_ipv4\":\"%s\",\"source\":\"https://api.ipify.org\"}", ip);
    __reply_text(id, result, send);
}

static const char *__wifi_status_name(WF_STATION_STAT_E status)
{
    switch (status) {
    case WSS_IDLE:
        return "disconnected";
    case WSS_CONNECTING:
        return "connecting";
    case WSS_PASSWD_WRONG:
        return "wrong_password";
    case WSS_NO_AP_FOUND:
        return "ap_not_found";
    case WSS_CONN_FAIL:
        return "connection_failed";
    case WSS_CONN_SUCCESS:
        return "obtaining_ip";
    case WSS_GOT_IP:
        return "connected";
    case WSS_DHCP_FAIL:
        return "dhcp_failed";
    default:
        return "unknown";
    }
}

static BOOL_T __usable_ip(const char *ip)
{
    return ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0;
}

/* Read only: no scanning, reconnect, DNS request or persistent network traffic.
 * Disconnected drivers can retain stale IP/RSSI values; never expose those as live. */
static void __network_status(cJSON *net)
{
    WF_STATION_STAT_E status = WSS_IDLE;
    NW_IP_S ip = {0};
    int8_t rssi = 0;
    char ssid[XZ_SSID_MAX + 1] = {0};
    char passwd[XZ_PASSWD_MAX + 1] = {0};
    const char *state = "unknown";

    if (OPRT_OK == tal_wifi_station_get_status(&status)) {
        state = __wifi_status_name(status);
    }
    BOOL_T known = strcmp(state, "unknown") != 0;
    BOOL_T connected = known && status == WSS_GOT_IP;
    BOOL_T associated = known && (status == WSS_CONN_SUCCESS || connected);
    BOOL_T service_up = connected && xz_ws_is_connected();
    BOOL_T configured = xz_settings_get_wifi(ssid, passwd);

    cJSON_AddStringToObject(net, "type", "wifi");
    cJSON_AddStringToObject(net, "status", state);
    if (known) {
        cJSON_AddBoolToObject(net, "connected", connected);
    } else {
        cJSON_AddNullToObject(net, "connected");
    }
    cJSON_AddStringToObject(net, "configured_ssid", configured ? ssid : "");
    cJSON_AddStringToObject(net, "ssid", associated && configured ? ssid : "");

    BOOL_T have_ip = connected && OPRT_OK == tal_wifi_get_ip(WF_STATION, &ip);
    if (have_ip && __usable_ip(ip.nwipstr)) {
        cJSON_AddStringToObject(net, "ip", ip.nwipstr);
    } else {
        cJSON_AddNullToObject(net, "ip");
    }
    if (have_ip && __usable_ip(ip.nwgwstr)) {
        cJSON_AddStringToObject(net, "gateway", ip.nwgwstr);
    } else {
        cJSON_AddNullToObject(net, "gateway");
    }
    if (associated && OPRT_OK == tal_wifi_station_get_conn_ap_rssi(&rssi) && rssi < 0) {
        cJSON_AddNumberToObject(net, "rssi_dbm", rssi);
        cJSON_AddStringToObject(net, "signal", rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak"));
    } else {
        cJSON_AddNullToObject(net, "rssi_dbm");
        cJSON_AddNullToObject(net, "signal");
    }
    cJSON_AddBoolToObject(net, "xiaozhi_connected", service_up);
    cJSON_AddStringToObject(net, "internet_status", service_up ? "xiaozhi_reachable" :
                            (connected ? "unverified" : (known ? "unavailable" : "unknown")));
}

static void __tool_network_status(int id, XZ_MCP_SEND_CB send)
{
    cJSON *root = cJSON_CreateObject();
    __network_status(root);
    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        PR_NOTICE("mcp: network status %s", str);
        __reply_text(id, str, send);
        cJSON_free(str);
    }
    cJSON_Delete(root);
}

static void __tool_device_status(int id, XZ_MCP_SEND_CB send)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *spk = cJSON_AddObjectToObject(root, "audio_speaker");
    XZ_BATTERY_T bat;
    char *str = NULL;

    cJSON_AddNumberToObject(spk, "volume", xz_audio_get_volume());

    if (xz_battery_get(&bat)) {
        cJSON *b = cJSON_AddObjectToObject(root, "battery");
        cJSON_AddNumberToObject(b, "level", bat.level);
        cJSON_AddBoolToObject(b, "charging", bat.charging);
        /* volts read more naturally than "4010 毫伏"; reads high while charging */
        cJSON_AddNumberToObject(b, "voltage", (double)((bat.mv + 5) / 10) / 100.0);
    }

    cJSON *net = cJSON_AddObjectToObject(root, "network");
    __network_status(net);

    str = cJSON_PrintUnformatted(root);
    if (str) {
        PR_NOTICE("mcp: device status %s", str);
        __reply_text(id, str, send);
        cJSON_free(str);
    }
    cJSON_Delete(root);
}

static void __tool_set_volume(int id, const cJSON *args, XZ_MCP_SEND_CB send)
{
    const cJSON *v = cJSON_IsObject(args) ? cJSON_GetObjectItem(args, "volume") : NULL;

    if (!cJSON_IsNumber(v)) {
        __reply_error(id, -32602, "Missing required argument: volume", send);
        return;
    }
    if (v->valueint < 0 || v->valueint > 100) {
        __reply_error(id, -32602, "volume must be between 0 and 100", send);
        return;
    }
    xz_audio_set_volume((uint8_t)v->valueint);
    xz_settings_set_int(XZ_KEY_VOLUME, v->valueint);
    PR_NOTICE("mcp: volume -> %d", v->valueint);
    __reply_text(id, "true", send);
}

void xz_mcp_handle(const cJSON *payload, XZ_MCP_SEND_CB send)
{
    const cJSON *method = cJSON_GetObjectItem(payload, "method");
    const cJSON *id = cJSON_GetObjectItem(payload, "id");
    const cJSON *params = cJSON_GetObjectItem(payload, "params");

    if (!cJSON_IsString(method)) {
        return; /* a response, nothing is ever requested by the device */
    }
    if (strncmp(method->valuestring, "notifications", 13) == 0) {
        return;
    }
    if (!cJSON_IsNumber(id)) {
        PR_WARN("mcp: %s without id", method->valuestring);
        return;
    }
    PR_DEBUG("mcp: %s", method->valuestring);

    if (strcmp(method->valuestring, "initialize") == 0) {
        __reply(id->valueint, "result",
                "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
                "\"serverInfo\":{\"name\":\"" XZ_BOARD_TYPE "\",\"version\":\"" PROJECT_VERSION "\"}}",
                send);
    } else if (strcmp(method->valuestring, "tools/list") == 0) {
        __reply(id->valueint, "result", sg_tools_json, send);
    } else if (strcmp(method->valuestring, "tools/call") == 0) {
        const cJSON *name = cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "name") : NULL;
        const cJSON *args = cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "arguments") : NULL;
        if (!cJSON_IsString(name)) {
            __reply_error(id->valueint, -32602, "Missing tool name", send);
            return;
        }
        PR_NOTICE("mcp: call %s", name->valuestring);
        if (strcmp(name->valuestring, "self.get_device_status") == 0) {
            __tool_device_status(id->valueint, send);
        } else if (strcmp(name->valuestring, "self.network.get_status") == 0) {
            __tool_network_status(id->valueint, send);
        } else if (strcmp(name->valuestring, "self.network.get_public_ip") == 0) {
            __tool_public_ip(id->valueint, send);
        } else if (strcmp(name->valuestring, "self.audio_speaker.set_volume") == 0) {
            __tool_set_volume(id->valueint, args, send);
        } else {
            __reply_error(id->valueint, -32602, "Unknown tool", send);
        }
    } else {
        __reply_error(id->valueint, -32601, "Method not implemented", send);
    }
}
