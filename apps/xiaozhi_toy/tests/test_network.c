/* Exercise real MCP JSON requests/replies with unavailable and stale Wi-Fi data. */
#include <assert.h>
#include "tal_wifi.h"
#include "xz_mcp.h"
#include "xz_battery.h"
#include "xz_settings.h"
#include "http_client_interface.h"
#include "xz_public_ip.h"

static WF_STATION_STAT_E station;
static int status_ret, rssi_ret, ip_ret, ip_reads, rssi_reads;
static int8_t signal_dbm;
static BOOL_T websocket, configured;
static NW_IP_S addresses;
static char reply[8192];
static const char *test_ssid = "test-\"wifi\\name";
static const uint8_t *http_body;
static size_t http_body_len;
static http_client_status_t http_result;
static uint16_t http_code;
static int http_calls, http_frees;

http_client_status_t http_client_request(const http_client_request_t *req, http_client_response_t *resp)
{
    http_calls++;
    assert(strcmp(req->host, "api.ipify.org") == 0 && req->port == 443);
    assert(strcmp(req->method, "GET") == 0 && strcmp(req->path, "/") == 0);
    assert(!req->tls_no_verify && req->cacert && req->cacert_len > 500);
    assert(req->cacert_len == strlen((const char *)req->cacert) + 1);
    assert(strstr((const char *)req->cacert, "-----BEGIN CERTIFICATE-----"));
    assert(req->timeout_ms == 5000 && req->body_length == 0);
    resp->status_code = http_code;
    resp->body = http_body;
    resp->body_length = http_body_len;
    return http_result;
}
int http_client_free(http_client_response_t *resp)
{
    (void)resp;
    http_frees++;
    return 0;
}

void *tal_malloc(size_t n) { return malloc(n); }
void tal_free(void *p) { free(p); }
OPERATE_RET tal_wifi_station_get_status(WF_STATION_STAT_E *status)
{
    *status = station;
    return status_ret;
}
OPERATE_RET tal_wifi_station_get_conn_ap_rssi(int8_t *rssi)
{
    rssi_reads++;
    *rssi = signal_dbm;
    return rssi_ret;
}
OPERATE_RET tal_wifi_get_ip(int wf, NW_IP_S *ip)
{
    (void)wf;
    ip_reads++;
    *ip = addresses;
    return ip_ret;
}
BOOL_T xz_ws_is_connected(void) { return websocket; }
BOOL_T xz_settings_get_wifi(char ssid[XZ_SSID_MAX + 1], char passwd[XZ_PASSWD_MAX + 1])
{
    snprintf(ssid, XZ_SSID_MAX + 1, "%s", test_ssid);
    snprintf(passwd, XZ_PASSWD_MAX + 1, "secret-do-not-return");
    return configured;
}
uint8_t xz_audio_get_volume(void) { return 55; }
void xz_audio_set_volume(uint8_t volume) { (void)volume; }
void xz_settings_set_int(const char *key, int value) { (void)key; (void)value; }
BOOL_T xz_battery_get(XZ_BATTERY_T *bat) { (void)bat; return FALSE; }

static void send_reply(const char *text)
{
    assert(strlen(text) < sizeof(reply));
    snprintf(reply, sizeof(reply), "%s", text);
}

static cJSON *request(const char *method, const char *tool)
{
    cJSON *q = cJSON_CreateObject();
    cJSON_AddStringToObject(q, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(q, "id", 42);
    cJSON_AddStringToObject(q, "method", method);
    if (tool) {
        cJSON *params = cJSON_AddObjectToObject(q, "params");
        cJSON_AddStringToObject(params, "name", tool);
    }
    reply[0] = 0;
    xz_mcp_handle(q, send_reply);
    cJSON_Delete(q);
    cJSON *answer = cJSON_Parse(reply);
    assert(answer && cJSON_GetObjectItem(answer, "id")->valueint == 42);
    assert(!strstr(reply, "secret-do-not-return"));
    return answer;
}

static cJSON *query(const char *tool)
{
    cJSON *answer = request("tools/call", tool);
    cJSON *result = cJSON_GetObjectItem(answer, "result");
    assert(cJSON_IsFalse(cJSON_GetObjectItem(result, "isError")));
    cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItem(result, "content"), 0);
    const cJSON *text = cJSON_GetObjectItem(item, "text");
    assert(cJSON_IsString(text));
    cJSON *data = cJSON_Parse(text->valuestring);
    assert(data);
    cJSON_Delete(answer);
    return data;
}

static void expect_string(cJSON *o, const char *key, const char *value)
{
    cJSON *item = cJSON_GetObjectItem(o, key);
    assert(cJSON_IsString(item) && strcmp(item->valuestring, value) == 0);
}

static void reset(void)
{
    station = WSS_GOT_IP;
    status_ret = rssi_ret = ip_ret = OPRT_OK;
    rssi_reads = ip_reads = 0;
    signal_dbm = -55;
    websocket = configured = TRUE;
    snprintf(addresses.nwipstr, sizeof(addresses.nwipstr), "192.168.1.8");
    snprintf(addresses.nwgwstr, sizeof(addresses.nwgwstr), "192.168.1.1");
    http_body = (const uint8_t *)"8.8.4.4";
    http_body_len = strlen((const char *)http_body);
    http_result = HTTP_CLIENT_SUCCESS;
    http_code = 200;
    http_calls = http_frees = 0;
}

int main(void)
{
    reset();
    cJSON *answer = request("tools/list", NULL);
    cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(answer, "result"), "tools");
    BOOL_T found = FALSE, public_ip_found = FALSE;
    cJSON *tool;
    cJSON_ArrayForEach(tool, tools) {
        if (strcmp(cJSON_GetObjectItem(tool, "name")->valuestring, "self.network.get_status") == 0) {
            found = TRUE;
            expect_string(cJSON_GetObjectItem(tool, "inputSchema"), "type", "object");
        }
        if (strcmp(cJSON_GetObjectItem(tool, "name")->valuestring, "self.network.get_public_ip") == 0) {
            public_ip_found = TRUE;
        }
    }
    assert(found && public_ip_found);
    cJSON_Delete(answer);

    cJSON *data = query("self.network.get_status");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(data, "connected")));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(data, "xiaozhi_connected")));
    expect_string(data, "ssid", test_ssid);
    expect_string(data, "ip", "192.168.1.8");
    expect_string(data, "gateway", "192.168.1.1");
    expect_string(data, "internet_status", "xiaozhi_reachable");
    assert(cJSON_GetObjectItem(data, "rssi_dbm")->valueint == -55);
    cJSON_Delete(data);

    websocket = FALSE;
    data = query("self.network.get_status");
    expect_string(data, "internet_status", "unverified");
    cJSON_Delete(data);

    const char *states[] = {"disconnected", "connecting", "wrong_password", "ap_not_found",
                            "connection_failed", "obtaining_ip", "connected", "dhcp_failed"};
    for (int i = WSS_IDLE; i <= WSS_DHCP_FAIL; i++) {
        reset();
        station = (WF_STATION_STAT_E)i;
        data = query("self.network.get_status");
        expect_string(data, "status", states[i]);
        if (station != WSS_GOT_IP) {
            assert(cJSON_IsFalse(cJSON_GetObjectItem(data, "connected")));
            assert(cJSON_IsFalse(cJSON_GetObjectItem(data, "xiaozhi_connected")));
            assert(cJSON_IsNull(cJSON_GetObjectItem(data, "ip")) && ip_reads == 0);
        }
        if (station != WSS_GOT_IP && station != WSS_CONN_SUCCESS) {
            assert(cJSON_IsNull(cJSON_GetObjectItem(data, "rssi_dbm")) && rssi_reads == 0);
            expect_string(data, "ssid", "");
        }
        cJSON_Delete(data);
    }

    for (int i = 0; i < 2; i++) {
        reset();
        if (i == 0) status_ret = OPRT_COM_ERROR;
        else station = (WF_STATION_STAT_E)99;
        data = query("self.network.get_status");
        expect_string(data, "status", "unknown");
        expect_string(data, "internet_status", "unknown");
        assert(cJSON_IsNull(cJSON_GetObjectItem(data, "connected")));
        assert(ip_reads == 0 && rssi_reads == 0);
        cJSON_Delete(data);
    }

    const int values[] = {-60, -61, -70, -71, 0, 12};
    const char *labels[] = {"strong", "medium", "medium", "weak", NULL, NULL};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        reset(); signal_dbm = (int8_t)values[i];
        data = query("self.network.get_status");
        if (labels[i]) expect_string(data, "signal", labels[i]);
        else assert(cJSON_IsNull(cJSON_GetObjectItem(data, "signal")));
        cJSON_Delete(data);
    }

    reset(); rssi_ret = ip_ret = OPRT_COM_ERROR;
    data = query("self.network.get_status");
    assert(cJSON_IsNull(cJSON_GetObjectItem(data, "rssi_dbm")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(data, "ip")));
    cJSON_Delete(data);

    reset(); strcpy(addresses.nwipstr, "0.0.0.0"); addresses.nwgwstr[0] = 0;
    data = query("self.network.get_status");
    assert(cJSON_IsNull(cJSON_GetObjectItem(data, "ip")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(data, "gateway")));
    cJSON_Delete(data);

    reset(); configured = FALSE;
    data = query("self.network.get_status");
    expect_string(data, "configured_ssid", ""); expect_string(data, "ssid", "");
    cJSON_Delete(data);

    reset();
    data = query("self.get_device_status");
    cJSON *network = cJSON_GetObjectItem(data, "network");
    expect_string(network, "status", "connected");
    expect_string(network, "internet_status", "xiaozhi_reachable");
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(data, "audio_speaker"), "volume")->valueint == 55);
    cJSON_Delete(data);
    assert(http_calls == 0); /* Ordinary status queries must not contact the IP service. */

    reset();
    data = query("self.network.get_public_ip");
    expect_string(data, "public_ipv4", "8.8.4.4");
    expect_string(data, "source", "https://api.ipify.org");
    assert(http_calls == 1 && http_frees == 1);
    cJSON_Delete(data);

    /* Every explicit query fetches again, including after a failed previous query. */
    http_body = (const uint8_t *)"  1.1.1.1\r\n";
    http_body_len = strlen((const char *)http_body);
    data = query("self.network.get_public_ip");
    expect_string(data, "public_ipv4", "1.1.1.1");
    assert(http_calls == 2 && http_frees == 2);
    cJSON_Delete(data);

    const char *invalid_ips[] = {"", "256.1.1.1", "1.2.3", "1.2.3.4.5", "1.2.3.4 bad", "01.2.3.4",
                                "10.1.2.3", "192.168.1.1", "172.16.1.1", "127.0.0.1", "0.0.0.0",
                                "100.64.0.1", "169.254.1.1", "224.0.0.1", "255.255.255.255",
                                "2001:4860:4860::8888", "<html>error</html>"};
    for (unsigned i = 0; i < sizeof(invalid_ips) / sizeof(invalid_ips[0]); i++) {
        reset();
        http_body = (const uint8_t *)invalid_ips[i];
        http_body_len = strlen(invalid_ips[i]);
        char ip[16] = "old-address";
        assert(xz_public_ip_query(ip) != OPRT_OK && ip[0] == 0);
        assert(http_frees == 1);
    }
    static const uint8_t nul_body[] = "8.8.8.8\0extra";
    reset(); http_body = nul_body; http_body_len = sizeof(nul_body) - 1;
    char ip[16];
    assert(xz_public_ip_query(ip) != OPRT_OK && ip[0] == 0);
    reset(); http_body_len = 65;
    assert(xz_public_ip_query(ip) != OPRT_OK && ip[0] == 0);

    for (int i = 0; i < 5; i++) {
        reset();
        if (i == 0) station = WSS_IDLE;
        if (i == 1) status_ret = OPRT_COM_ERROR;
        if (i == 2) http_result = HTTP_CLIENT_SEND_FAULT;
        if (i == 3) http_code = 503;
        if (i == 4) http_code = 302;
        answer = request("tools/call", "self.network.get_public_ip");
        cJSON *result = cJSON_GetObjectItem(answer, "result");
        assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "isError")));
        assert(strstr(reply, "lookup failed") && !strstr(reply, "public_ipv4"));
        if (i < 2) assert(http_calls == 0 && http_frees == 0);
        else assert(http_calls == 1 && http_frees == 1);
        cJSON_Delete(answer);
    }
    puts("Network MCP regression checks passed");
    return 0;
}
