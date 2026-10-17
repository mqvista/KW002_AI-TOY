/**
 * @file xz_dns.h
 * @brief Tiny captive DNS server: answers every A query with the SoftAP address so
 *        phones open the provisioning page automatically.
 */

#ifndef __XZ_DNS_H__
#define __XZ_DNS_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

OPERATE_RET xz_dns_start(const char *ap_ip);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_DNS_H__ */
