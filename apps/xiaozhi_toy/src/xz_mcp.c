/**
 * @file xz_mcp.c
 * @brief Device side MCP server, compatible with xiaozhi-esp32 (main/mcp_server.cc).
 *
 * The server sends {"type":"mcp","payload":<JSON-RPC 2.0>} after the hello when the device
 * announced "features":{"mcp":true}: initialize, tools/list, then tools/call whenever the
 * LLM decides to use a tool. Tool names and schemas follow xiaozhi-esp32 so the official
 * server prompts work unchanged:
 *   self.get_device_status         volume, battery level / charging, Wi-Fi
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

static const char sg_tools_json[] =
    "{\"tools\":["
    "{\"name\":\"self.get_device_status\","
    "\"description\":\"Provides the real-time information of the device, including the current status of the "
    "audio speaker, battery, network, etc.\\nUse this tool for: \\n1. Answering questions about current "
    "condition (e.g. what is the current volume of the audio speaker? how much battery is left? is it "
    "charging?)\\n2. As the first step to control the device (e.g. turn up / down the volume of the audio "
    "speaker, etc.)\","
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
static void __reply_text(int id, const char *text, XZ_MCP_SEND_CB send)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = cJSON_CreateObject();
    char *str = NULL;

    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(result, "isError", FALSE);
    str = cJSON_PrintUnformatted(result);
    if (str) {
        __reply(id, "result", str, send);
        cJSON_free(str);
    }
    cJSON_Delete(result);
}

static void __tool_device_status(int id, XZ_MCP_SEND_CB send)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *spk = cJSON_AddObjectToObject(root, "audio_speaker");
    XZ_BATTERY_T bat;
    char ssid[XZ_SSID_MAX + 1] = {0};
    char passwd[XZ_PASSWD_MAX + 1] = {0};
    int8_t rssi = 0;
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
    cJSON_AddStringToObject(net, "type", "wifi");
    xz_settings_get_wifi(ssid, passwd);
    cJSON_AddStringToObject(net, "ssid", ssid);
    if (OPRT_OK == tal_wifi_station_get_conn_ap_rssi(&rssi)) {
        cJSON_AddStringToObject(net, "signal", rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak"));
    }

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
        } else if (strcmp(name->valuestring, "self.audio_speaker.set_volume") == 0) {
            __tool_set_volume(id->valueint, args, send);
        } else {
            __reply_error(id->valueint, -32602, "Unknown tool", send);
        }
    } else {
        __reply_error(id->valueint, -32601, "Method not implemented", send);
    }
}
