/**
 * @file xz_ws.h
 * @brief Minimal WebSocket client (RFC 6455) over TLS with custom handshake headers.
 */

#ifndef __XZ_WS_H__
#define __XZ_WS_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    const char *value;
} XZ_WS_HEADER_T;

typedef struct {
    void (*on_text)(const char *text, uint32_t len);       /* NUL terminated */
    void (*on_binary)(const uint8_t *data, uint32_t len);
    void (*on_closed)(void);                                /* connection lost or closed by peer */
} XZ_WS_CB_T;

/** Connect to ws:// or wss:// url and start the I/O thread. */
OPERATE_RET xz_ws_connect(const char *url, const XZ_WS_HEADER_T *headers, uint32_t header_cnt, const XZ_WS_CB_T *cb);

OPERATE_RET xz_ws_send_text(const char *text);
OPERATE_RET xz_ws_send_binary(const uint8_t *data, uint32_t len);

/** Send a close frame, stop the I/O thread and free everything. Safe to call twice. */
void xz_ws_close(void);

BOOL_T xz_ws_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_WS_H__ */
