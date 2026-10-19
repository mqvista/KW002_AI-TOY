/** Public IPv4 lookup. API: https://www.ipify.org/ (plain-text IPv4 endpoint). */
#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_wifi.h"
#include "http_client_interface.h"
#include "xz_certs.h"
#include "xz_public_ip.h"

#define PUBLIC_IP_TIMEOUT_MS 5000U

static BOOL_T __space(uint8_t c)
{
    return c == ' ' || c == '\r' || c == '\n' || c == '\t';
}

/* Parse a length-delimited body, including rejecting embedded NULs/trailing data.
 * Do not accept a LAN, loopback, link-local or carrier NAT address as public. */
static BOOL_T __parse_address(const uint8_t *body, size_t len, char out[16])
{
    unsigned octets[4];
    size_t pos = 0;
    if (!body || len == 0 || len > 64) {
        return FALSE;
    }
    while (pos < len && __space(body[pos])) {
        pos++;
    }
    for (unsigned i = 0; i < 4; i++) {
        size_t start = pos;
        unsigned value = 0;
        while (pos < len && body[pos] >= '0' && body[pos] <= '9') {
            value = value * 10 + (body[pos++] - '0');
            if (pos - start > 3 || value > 255) {
                return FALSE;
            }
        }
        if (pos == start || (pos - start > 1 && body[start] == '0')) {
            return FALSE;
        }
        octets[i] = value;
        if (i < 3 && (pos == len || body[pos++] != '.')) {
            return FALSE;
        }
    }
    while (pos < len && __space(body[pos])) {
        pos++;
    }
    if (pos != len || octets[0] == 0 || octets[0] == 10 || octets[0] == 127 || octets[0] >= 224 ||
        (octets[0] == 100 && octets[1] >= 64 && octets[1] <= 127) ||
        (octets[0] == 169 && octets[1] == 254) ||
        (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
        (octets[0] == 192 && octets[1] == 168)) {
        return FALSE;
    }
    snprintf(out, 16, "%u.%u.%u.%u", octets[0], octets[1], octets[2], octets[3]);
    return TRUE;
}

OPERATE_RET xz_public_ip_query(char out[16])
{
    WF_STATION_STAT_E status = WSS_IDLE;
    http_client_response_t response = {0};
    http_client_header_t headers[] = {{"Accept", "text/plain"}, {"Connection", "close"}};
    out[0] = '\0';
    if (OPRT_OK != tal_wifi_station_get_status(&status) || status != WSS_GOT_IP) {
        return OPRT_RESOURCE_NOT_READY;
    }
    http_client_status_t result = http_client_request(
        &(const http_client_request_t){
            .host = "api.ipify.org",
            .port = 443,
            .path = "/",
            .method = "GET",
            .cacert = (const uint8_t *)g_xz_public_ip_ca_pem,
            .cacert_len = strlen(g_xz_public_ip_ca_pem) + 1,
            .headers = headers,
            .headers_count = sizeof(headers) / sizeof(headers[0]),
            .timeout_ms = PUBLIC_IP_TIMEOUT_MS,
        }, &response);
    BOOL_T valid = result == HTTP_CLIENT_SUCCESS && response.status_code == 200 &&
                   __parse_address(response.body, response.body_length, out);
    http_client_free(&response);
    if (!valid) {
        PR_WARN("public IPv4 lookup failed (transport %d, HTTP %u)", result, response.status_code);
        return OPRT_COM_ERROR;
    }
    return OPRT_OK;
}
