/** On-demand lookup of the network's public IPv4 egress address. */
#ifndef __XZ_PUBLIC_IP_H__
#define __XZ_PUBLIC_IP_H__

#include "tuya_cloud_types.h"

/** out is empty on failure. No cached result or background requests. */
OPERATE_RET xz_public_ip_query(char out[16]);

#endif
