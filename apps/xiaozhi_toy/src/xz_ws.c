/**
 * @file xz_ws.c
 * @brief Minimal WebSocket client (RFC 6455) over TLS with custom handshake headers.
 *
 * One I/O thread owns the TLS connection: it writes frames queued by xz_ws_send_*()
 * and polls for incoming data, so the TLS context is never used from two threads.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tal_api.h"
#include "tuya_transporter.h"
#include "tuya_tls.h"

#include "xz_certs.h"
#include "xz_ws.h"

/***********************************************************
************************macro define************************
***********************************************************/
#define WS_CONNECT_TIMEOUT_MS 10000
#define WS_IO_TIMEOUT_MS      5000
#define WS_POLL_MS            10
#define WS_RX_BUF_SIZE        (16 * 1024)
#define WS_TX_QUEUE_DEPTH     64
#define WS_HANDSHAKE_MAX      2048

#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    uint8_t opcode;
    uint32_t len;
    uint8_t data[];
} WS_TX_T;

/***********************************************************
***********************variable define**********************
***********************************************************/
static tuya_transporter_t sg_tp = NULL;
static QUEUE_HANDLE sg_tx_queue = NULL;
static THREAD_HANDLE sg_io_thread = NULL;
static volatile BOOL_T sg_running = FALSE;
static volatile BOOL_T sg_connected = FALSE;
static XZ_WS_CB_T sg_cb = {0};
static uint8_t *sg_rx = NULL;
static uint32_t sg_rx_fill = 0;
static uint8_t *sg_msg = NULL; /* reassembly of fragmented messages */
static uint32_t sg_msg_len = 0;
static uint8_t sg_msg_op = 0;

/***********************************************************
***********************function define**********************
***********************************************************/
static void __base64(const uint8_t *in, uint32_t len, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t i, o = 0;

    for (i = 0; i + 2 < len; i += 3) {
        out[o++] = tbl[in[i] >> 2];
        out[o++] = tbl[((in[i] & 0x03) << 4) | (in[i + 1] >> 4)];
        out[o++] = tbl[((in[i + 1] & 0x0F) << 2) | (in[i + 2] >> 6)];
        out[o++] = tbl[in[i + 2] & 0x3F];
    }
    if (i < len) {
        out[o++] = tbl[in[i] >> 2];
        if (i + 1 < len) {
            out[o++] = tbl[((in[i] & 0x03) << 4) | (in[i + 1] >> 4)];
            out[o++] = tbl[(in[i + 1] & 0x0F) << 2];
        } else {
            out[o++] = tbl[(in[i] & 0x03) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = '\0';
}

static OPERATE_RET __write_all(const uint8_t *buf, uint32_t len)
{
    while (len > 0) {
        int n = tuya_transporter_write(sg_tp, (uint8_t *)buf, (int)len, WS_IO_TIMEOUT_MS);
        if (n <= 0) {
            PR_ERR("ws write error %d", n);
            return OPRT_SEND_ERR;
        }
        buf += n;
        len -= (uint32_t)n;
    }
    return OPRT_OK;
}

static OPERATE_RET __send_frame(uint8_t opcode, const uint8_t *payload, uint32_t len)
{
    uint8_t hdr[14];
    uint32_t h = 0;
    uint8_t mask[4];
    uint8_t chunk[256];
    OPERATE_RET rt;

    hdr[h++] = 0x80 | opcode; /* FIN */
    if (len < 126) {
        hdr[h++] = 0x80 | (uint8_t)len;
    } else if (len <= 0xFFFF) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)len;
    } else {
        hdr[h++] = 0x80 | 127;
        hdr[h++] = 0;
        hdr[h++] = 0;
        hdr[h++] = 0;
        hdr[h++] = 0;
        hdr[h++] = (uint8_t)(len >> 24);
        hdr[h++] = (uint8_t)(len >> 16);
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)len;
    }
    for (int i = 0; i < 4; i++) {
        mask[i] = (uint8_t)tal_system_get_random(0xFF);
        hdr[h++] = mask[i];
    }
    rt = __write_all(hdr, h);
    if (OPRT_OK != rt) {
        return rt;
    }

    /* client frames must be masked; mask through a small stack buffer */
    for (uint32_t off = 0; off < len;) {
        uint32_t n = (len - off) > sizeof(chunk) ? sizeof(chunk) : (len - off);
        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = payload[off + i] ^ mask[(off + i) & 3];
        }
        rt = __write_all(chunk, n);
        if (OPRT_OK != rt) {
            return rt;
        }
        off += n;
    }
    return OPRT_OK;
}

static OPERATE_RET __enqueue(uint8_t opcode, const uint8_t *data, uint32_t len)
{
    WS_TX_T *tx;

    if (!sg_connected) {
        return OPRT_RESOURCE_NOT_READY;
    }
    tx = tal_malloc(sizeof(WS_TX_T) + len);
    if (NULL == tx) {
        return OPRT_MALLOC_FAILED;
    }
    tx->opcode = opcode;
    tx->len = len;
    if (len) {
        memcpy(tx->data, data, len);
    }
    if (OPRT_OK != tal_queue_post(sg_tx_queue, &tx, 0)) {
        tal_free(tx);
        PR_WARN("ws tx queue full, frame dropped");
        return OPRT_COM_ERROR;
    }
    return OPRT_OK;
}

static void __deliver(uint8_t opcode, uint8_t *data, uint32_t len)
{
    if (WS_OP_TEXT == opcode) {
        if (sg_cb.on_text) {
            char saved = (char)data[len]; /* there is always room after the payload */
            data[len] = '\0';
            sg_cb.on_text((const char *)data, len);
            data[len] = (uint8_t)saved;
        }
    } else if (WS_OP_BINARY == opcode) {
        if (sg_cb.on_binary) {
            sg_cb.on_binary(data, len);
        }
    }
}

/* Parse complete frames in the rx buffer. Returns FALSE when the connection must close. */
static BOOL_T __parse_frames(void)
{
    while (sg_rx_fill >= 2) {
        uint8_t b0 = sg_rx[0], b1 = sg_rx[1];
        uint8_t opcode = b0 & 0x0F;
        BOOL_T fin = (b0 & 0x80) != 0;
        BOOL_T masked = (b1 & 0x80) != 0;
        uint64_t plen = b1 & 0x7F;
        uint32_t hlen = 2;

        if (plen == 126) {
            if (sg_rx_fill < 4) {
                return TRUE;
            }
            plen = (uint64_t)sg_rx[2] << 8 | sg_rx[3];
            hlen = 4;
        } else if (plen == 127) {
            if (sg_rx_fill < 10) {
                return TRUE;
            }
            plen = 0;
            for (int i = 0; i < 8; i++) {
                plen = plen << 8 | sg_rx[2 + i];
            }
            hlen = 10;
        }
        if (masked) {
            hlen += 4;
        }
        if (hlen + plen + 1 > WS_RX_BUF_SIZE) {
            PR_ERR("ws frame too large: %llu", (unsigned long long)plen);
            return FALSE;
        }
        if (sg_rx_fill < hlen + plen) {
            return TRUE; /* wait for the rest */
        }

        uint8_t *payload = sg_rx + hlen;
        uint32_t len = (uint32_t)plen;
        if (masked) {
            uint8_t *m = sg_rx + hlen - 4;
            for (uint32_t i = 0; i < len; i++) {
                payload[i] ^= m[i & 3];
            }
        }

        switch (opcode) {
        case WS_OP_TEXT:
        case WS_OP_BINARY:
            if (fin) {
                __deliver(opcode, payload, len);
            } else {
                sg_msg_op = opcode;
                sg_msg_len = 0;
                goto __append;
            }
            break;
        case WS_OP_CONT:
        __append:
            if (sg_msg_len + len + 1 > WS_RX_BUF_SIZE) {
                PR_ERR("ws fragmented message too large");
                return FALSE;
            }
            memcpy(sg_msg + sg_msg_len, payload, len);
            sg_msg_len += len;
            if (fin) {
                __deliver(sg_msg_op, sg_msg, sg_msg_len);
                sg_msg_len = 0;
            }
            break;
        case WS_OP_PING:
            __send_frame(WS_OP_PONG, payload, len);
            break;
        case WS_OP_CLOSE:
            PR_NOTICE("ws closed by server, code %u", len >= 2 ? (unsigned)(payload[0] << 8 | payload[1]) : 0);
            __send_frame(WS_OP_CLOSE, payload, len >= 2 ? 2 : 0);
            return FALSE;
        default:
            break;
        }

        /* drop the consumed frame */
        uint32_t used = hlen + len;
        memmove(sg_rx, sg_rx + used, sg_rx_fill - used);
        sg_rx_fill -= used;
    }
    return TRUE;
}

static void __io_task(void *arg)
{
    WS_TX_T *tx = NULL;
    BOOL_T lost = FALSE;

    (void)arg;
    /* frames that arrived together with the handshake response */
    if (sg_rx_fill > 0 && !__parse_frames()) {
        lost = TRUE;
    }
    while (sg_running && !lost) {
        /* 1. flush queued frames */
        while (OPRT_OK == tal_queue_fetch(sg_tx_queue, &tx, 0)) {
            OPERATE_RET rt = __send_frame(tx->opcode, tx->data, tx->len);
            tal_free(tx);
            if (OPRT_OK != rt) {
                lost = TRUE;
                break;
            }
        }
        if (lost) {
            break;
        }

        /* 2. read what is available */
        int n = tuya_transporter_read(sg_tp, sg_rx + sg_rx_fill, (int)(WS_RX_BUF_SIZE - 1 - sg_rx_fill), WS_POLL_MS);
        if (n > 0) {
            sg_rx_fill += (uint32_t)n;
            if (!__parse_frames()) {
                lost = TRUE;
                break;
            }
        } else if (n == 0 || (n < 0 && n != OPRT_RESOURCE_NOT_READY && n != OPRT_TIMEOUT)) {
            PR_ERR("ws read error %d", n);
            lost = TRUE;
            break;
        }
    }

    sg_connected = FALSE;
    if (lost && sg_running && sg_cb.on_closed) {
        sg_cb.on_closed();
    }
    sg_running = FALSE;
    tal_thread_delete(sg_io_thread);
    sg_io_thread = NULL;
}

/* "wss://host[:port]/path" */
static OPERATE_RET __parse_url(const char *url, BOOL_T *tls, char *host, size_t host_len, uint16_t *port, char *path,
                               size_t path_len)
{
    const char *p;
    const char *slash;
    const char *colon;

    if (strncmp(url, "wss://", 6) == 0) {
        *tls = TRUE;
        *port = 443;
        p = url + 6;
    } else if (strncmp(url, "ws://", 5) == 0) {
        *tls = FALSE;
        *port = 80;
        p = url + 5;
    } else {
        return OPRT_INVALID_PARM;
    }

    slash = strchr(p, '/');
    colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        *port = (uint16_t)atoi(colon + 1);
    } else {
        colon = slash ? slash : p + strlen(p);
    }
    if ((size_t)(colon - p) >= host_len) {
        return OPRT_INVALID_PARM;
    }
    memcpy(host, p, colon - p);
    host[colon - p] = '\0';
    snprintf(path, path_len, "%s", slash ? slash : "/");
    return OPRT_OK;
}

static OPERATE_RET __handshake(const char *host, uint16_t port, const char *path, const XZ_WS_HEADER_T *headers,
                               uint32_t header_cnt)
{
    char *req = tal_malloc(WS_HANDSHAKE_MAX);
    uint8_t key_raw[16];
    char key[32];
    int len = 0;
    OPERATE_RET rt = OPRT_OK;

    if (NULL == req) {
        return OPRT_MALLOC_FAILED;
    }
    for (int i = 0; i < 16; i++) {
        key_raw[i] = (uint8_t)tal_system_get_random(0xFF);
    }
    __base64(key_raw, sizeof(key_raw), key);

    len = snprintf(req, WS_HANDSHAKE_MAX,
                   "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n",
                   path, host, key);
    (void)port;
    for (uint32_t i = 0; i < header_cnt && len < WS_HANDSHAKE_MAX; i++) {
        len += snprintf(req + len, WS_HANDSHAKE_MAX - len, "%s: %s\r\n", headers[i].key, headers[i].value);
    }
    len += snprintf(req + len, WS_HANDSHAKE_MAX - len, "\r\n");
    if (len >= WS_HANDSHAKE_MAX) {
        tal_free(req);
        return OPRT_BUFFER_NOT_ENOUGH;
    }

    rt = __write_all((const uint8_t *)req, (uint32_t)len);
    if (OPRT_OK != rt) {
        tal_free(req);
        return rt;
    }

    /* read the response header */
    len = 0;
    SYS_TIME_T deadline = tal_system_get_millisecond() + WS_CONNECT_TIMEOUT_MS;
    while (tal_system_get_millisecond() < deadline && len < WS_HANDSHAKE_MAX - 1) {
        int n = tuya_transporter_read(sg_tp, (uint8_t *)req + len, WS_HANDSHAKE_MAX - 1 - len, 200);
        if (n > 0) {
            len += n;
            req[len] = '\0';
            char *end = strstr(req, "\r\n\r\n");
            if (end) {
                /* keep any frame bytes that arrived with the header */
                uint32_t hdr_len = (uint32_t)(end + 4 - req);
                sg_rx_fill = (uint32_t)len - hdr_len;
                memcpy(sg_rx, req + hdr_len, sg_rx_fill);
                break;
            }
        } else if (n == 0 || (n < 0 && n != OPRT_RESOURCE_NOT_READY && n != OPRT_TIMEOUT)) {
            break;
        }
    }

    if (strncmp(req, "HTTP/1.1 101", 12) != 0) {
        char *eol = strstr(req, "\r\n");
        if (eol) {
            *eol = '\0';
        }
        PR_ERR("ws handshake failed: %s", len > 0 ? req : "(no response)");
        rt = OPRT_COM_ERROR;
    }
    tal_free(req);
    return rt;
}

static void __cleanup(void)
{
    WS_TX_T *tx = NULL;

    if (sg_tp) {
        tuya_transporter_close(sg_tp);
        tuya_transporter_destroy(sg_tp);
        sg_tp = NULL;
    }
    if (sg_tx_queue) {
        while (OPRT_OK == tal_queue_fetch(sg_tx_queue, &tx, 0)) {
            tal_free(tx);
        }
    }
    sg_rx_fill = 0;
    sg_msg_len = 0;
}

OPERATE_RET xz_ws_connect(const char *url, const XZ_WS_HEADER_T *headers, uint32_t header_cnt, const XZ_WS_CB_T *cb)
{
    OPERATE_RET rt = OPRT_OK;
    BOOL_T tls = TRUE;
    char host[64];
    char path[128];
    uint16_t port = 443;

    xz_ws_close();
    TUYA_CALL_ERR_RETURN(__parse_url(url, &tls, host, sizeof(host), &port, path, sizeof(path)));

    if (NULL == sg_tx_queue) {
        TUYA_CALL_ERR_RETURN(tal_queue_create_init(&sg_tx_queue, sizeof(WS_TX_T *), WS_TX_QUEUE_DEPTH));
    }
    if (NULL == sg_rx) {
        sg_rx = tal_malloc(WS_RX_BUF_SIZE);
        sg_msg = tal_malloc(WS_RX_BUF_SIZE);
        if (NULL == sg_rx || NULL == sg_msg) {
            return OPRT_MALLOC_FAILED;
        }
    }
    sg_cb = *cb;

    sg_tp = tuya_transporter_create(tls ? TRANSPORT_TYPE_TLS : TRANSPORT_TYPE_TCP, NULL);
    if (NULL == sg_tp) {
        return OPRT_MALLOC_FAILED;
    }
    if (tls) {
        tuya_tls_config_t tls_cfg = {
            .mode = TUYA_TLS_SERVER_CERT_MODE,
            .hostname = host,
            .port = port,
            .timeout = WS_CONNECT_TIMEOUT_MS,
            .verify = true,
            .ca_cert = (char *)g_xz_root_ca_pem,
            .ca_cert_size = (int)strlen(g_xz_root_ca_pem) + 1,
        };
        rt = tuya_transporter_ctrl(sg_tp, TUYA_TRANSPORTER_SET_TLS_CONFIG, &tls_cfg);
        if (OPRT_OK != rt) {
            goto __FAIL;
        }
    }

    PR_NOTICE("ws connecting %s:%u%s", host, port, path);
    rt = tuya_transporter_connect(sg_tp, host, port, WS_CONNECT_TIMEOUT_MS);
    if (OPRT_OK != rt) {
        PR_ERR("ws connect failed %d", rt);
        goto __FAIL;
    }
    rt = __handshake(host, port, path, headers, header_cnt);
    if (OPRT_OK != rt) {
        goto __FAIL;
    }

    sg_connected = TRUE;
    sg_running = TRUE;
    THREAD_CFG_T cfg = {.stackDepth = 12 * 1024, .priority = THREAD_PRIO_2, .thrdname = "xz_ws"};
    rt = tal_thread_create_and_start(&sg_io_thread, NULL, NULL, __io_task, NULL, &cfg);
    if (OPRT_OK != rt) {
        sg_connected = FALSE;
        sg_running = FALSE;
        goto __FAIL;
    }
    PR_NOTICE("ws connected");
    return OPRT_OK;

__FAIL:
    __cleanup();
    return rt;
}

OPERATE_RET xz_ws_send_text(const char *text)
{
    return __enqueue(WS_OP_TEXT, (const uint8_t *)text, strlen(text));
}

OPERATE_RET xz_ws_send_binary(const uint8_t *data, uint32_t len)
{
    return __enqueue(WS_OP_BINARY, data, len);
}

void xz_ws_close(void)
{
    if (sg_running) {
        uint8_t code[2] = {0x03, 0xE8}; /* 1000 normal closure */
        __enqueue(WS_OP_CLOSE, code, sizeof(code));
        tal_system_sleep(50);
        sg_running = FALSE;
        for (int i = 0; i < 100 && sg_io_thread; i++) {
            tal_system_sleep(10);
        }
    }
    sg_connected = FALSE;
    __cleanup();
}

BOOL_T xz_ws_is_connected(void)
{
    return sg_connected;
}
