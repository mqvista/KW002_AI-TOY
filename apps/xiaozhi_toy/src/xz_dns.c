/**
 * @file xz_dns.c
 * @brief Tiny captive DNS server: answers every A query with the SoftAP address so
 *        phones open the provisioning page automatically.
 */

#include <string.h>

#include "tal_api.h"
#include "tal_network.h"

#include "xz_dns.h"

#define DNS_PORT     53
#define DNS_BUF_SIZE 512
#define DNS_HDR_LEN  12

static THREAD_HANDLE sg_dns_thread = NULL;
static TUYA_IP_ADDR_T sg_ap_addr = 0;

/* Build the answer in place; returns the response length or 0 to drop the packet. */
static uint32_t __dns_build_reply(uint8_t *buf, uint32_t len)
{
    uint32_t pos = DNS_HDR_LEN;
    uint16_t qtype = 0;
    uint32_t ip = 0;

    if (len < DNS_HDR_LEN || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) {
        return 0; /* not a single question query */
    }

    /* skip QNAME */
    while (pos < len && buf[pos] != 0) {
        pos += buf[pos] + 1;
    }
    pos += 1;
    if (pos + 4 > len) {
        return 0;
    }
    qtype = (uint16_t)(buf[pos] << 8 | buf[pos + 1]);
    pos += 4; /* QTYPE + QCLASS */

    buf[2] = 0x81; /* response, recursion desired */
    buf[3] = 0x80; /* recursion available, no error */
    buf[6] = 0;    /* ANCOUNT */
    buf[7] = 0;
    buf[8] = buf[9] = buf[10] = buf[11] = 0; /* NSCOUNT, ARCOUNT */

    if (qtype != 1 /* A */ || pos + 16 > DNS_BUF_SIZE) {
        return pos; /* empty answer, e.g. AAAA */
    }

    buf[7] = 1;
    buf[pos++] = 0xC0; /* name: pointer to the question */
    buf[pos++] = 0x0C;
    buf[pos++] = 0x00; /* type A */
    buf[pos++] = 0x01;
    buf[pos++] = 0x00; /* class IN */
    buf[pos++] = 0x01;
    buf[pos++] = 0x00; /* TTL 60 s */
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;
    buf[pos++] = 0x3C;
    buf[pos++] = 0x00; /* RDLENGTH 4 */
    buf[pos++] = 0x04;
    ip = TUYA_IP_ADDR_GET_IP4(sg_ap_addr); /* tal_net addresses are host byte order */
    buf[pos++] = (uint8_t)(ip >> 24);
    buf[pos++] = (uint8_t)(ip >> 16);
    buf[pos++] = (uint8_t)(ip >> 8);
    buf[pos++] = (uint8_t)(ip);
    return pos;
}

static void __dns_task(void *arg)
{
    uint8_t buf[DNS_BUF_SIZE];
    int fd = tal_net_socket_create(PROTOCOL_UDP);

    (void)arg;
    if (fd < 0) {
        PR_ERR("dns socket create failed");
        goto __EXIT;
    }
    tal_net_set_reuse(fd);
    if (tal_net_bind(fd, TY_IPADDR_ANY, DNS_PORT) < 0) {
        PR_ERR("dns bind failed %d", tal_net_get_errno());
        tal_net_close(fd);
        goto __EXIT;
    }
    PR_NOTICE("captive dns started");

    while (1) {
        TUYA_IP_ADDR_T from = TY_IPADDR_ANY;
        uint16_t port = 0;
        int n = tal_net_recvfrom(fd, buf, sizeof(buf), &from, &port);
        if (n <= 0) {
            tal_system_sleep(10);
            continue;
        }
        uint32_t out = __dns_build_reply(buf, (uint32_t)n);
        if (out > 0) {
            tal_net_send_to(fd, buf, out, from, port);
        }
    }

__EXIT:
    tal_thread_delete(sg_dns_thread);
    sg_dns_thread = NULL;
}

OPERATE_RET xz_dns_start(const char *ap_ip)
{
    THREAD_CFG_T cfg = {.stackDepth = 3 * 1024, .priority = THREAD_PRIO_2, .thrdname = "xz_dns"};

    sg_ap_addr = tal_net_str2addr(ap_ip);
    return tal_thread_create_and_start(&sg_dns_thread, NULL, NULL, __dns_task, NULL, &cfg);
}
