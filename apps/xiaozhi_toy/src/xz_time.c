/**
 * @file xz_time.c
 * @brief Wall clock sync (SNTP). TLS certificate checks need a sane date.
 *
 * mbedtls takes the date from libc time(); the device boots in 1970, which makes
 * every certificate "not yet valid". Set both the libc clock and TuyaOpen's tal_time.
 */

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "tal_api.h"
#include "tal_network.h"
#include "tal_time_service.h"

#include "xz_time.h"

#define NTP_PORT        123
#define NTP_UNIX_OFFSET 2208988800UL /* 1900 -> 1970 */
#define NTP_TIMEOUT_MS  3000

/* Beken AON RTC wall clock (platform driver/aon_rtc.h); newlib settimeofday() is not provided */
extern int bk_rtc_settimeofday(const struct timeval *tv, const struct timezone *tz);

static const char *sg_ntp_servers[] = {"ntp.aliyun.com", "ntp.tencent.com", "cn.pool.ntp.org", "pool.ntp.org"};

void xz_time_set_ms(int64_t unix_ms)
{
    struct timeval tv = {.tv_sec = (time_t)(unix_ms / 1000), .tv_usec = (suseconds_t)((unix_ms % 1000) * 1000)};
    long long before = (long long)time(NULL);

    bk_rtc_settimeofday(&tv, NULL);
    tal_time_set_posix((TIME_T)(unix_ms / 1000), 1);
    PR_NOTICE("clock set to %lld, libc time() %lld -> %lld", (long long)(unix_ms / 1000), before,
              (long long)time(NULL));
}

static OPERATE_RET __ntp_query(const char *server, uint32_t *unix_sec)
{
    TUYA_IP_ADDR_T addr = TY_IPADDR_ANY;
    uint8_t pkt[48] = {0};
    int fd = -1;
    int n;

    if (OPRT_OK != tal_net_gethostbyname(server, &addr)) {
        PR_WARN("ntp: resolve %s failed", server);
        return OPRT_COM_ERROR;
    }
    fd = tal_net_socket_create(PROTOCOL_UDP);
    if (fd < 0) {
        return OPRT_SOCK_ERR;
    }
    tal_net_set_timeout(fd, NTP_TIMEOUT_MS, TRANS_RECV);

    pkt[0] = 0x1B; /* LI 0, version 3, client */
    if (tal_net_send_to(fd, pkt, sizeof(pkt), addr, NTP_PORT) < 0) {
        tal_net_close(fd);
        return OPRT_SEND_ERR;
    }
    n = tal_net_recv(fd, pkt, sizeof(pkt));
    tal_net_close(fd);
    if (n < 48) {
        PR_WARN("ntp: no answer from %s", server);
        return OPRT_TIMEOUT;
    }

    /* transmit timestamp, seconds part, big endian */
    uint32_t secs = (uint32_t)pkt[40] << 24 | (uint32_t)pkt[41] << 16 | (uint32_t)pkt[42] << 8 | pkt[43];
    if (secs < NTP_UNIX_OFFSET) {
        return OPRT_COM_ERROR;
    }
    *unix_sec = secs - NTP_UNIX_OFFSET;
    return OPRT_OK;
}

OPERATE_RET xz_time_sync(void)
{
    for (uint32_t i = 0; i < sizeof(sg_ntp_servers) / sizeof(sg_ntp_servers[0]); i++) {
        uint32_t now = 0;
        if (OPRT_OK == __ntp_query(sg_ntp_servers[i], &now)) {
            PR_NOTICE("ntp: %s -> %u", sg_ntp_servers[i], now);
            xz_time_set_ms((int64_t)now * 1000);
            return OPRT_OK;
        }
    }
    PR_ERR("ntp: all servers failed");
    return OPRT_COM_ERROR;
}
