#ifndef TEST_HTTP_CLIENT_INTERFACE_H
#define TEST_HTTP_CLIENT_INTERFACE_H
#include <stdbool.h>
#include "test_sdk.h"
typedef enum { HTTP_CLIENT_SUCCESS, HTTP_CLIENT_SEND_FAULT } http_client_status_t;
typedef struct { const char *key; const char *value; } http_client_header_t;
typedef struct {
    const char *host;
    uint16_t port;
    const char *path;
    const uint8_t *cacert;
    size_t cacert_len;
    bool tls_no_verify;
    const char *method;
    http_client_header_t *headers;
    uint8_t headers_count;
    const uint8_t *body;
    size_t body_length;
    uint32_t timeout_ms;
} http_client_request_t;
typedef struct {
    const uint8_t *body;
    size_t body_length;
    uint16_t status_code;
} http_client_response_t;
http_client_status_t http_client_request(const http_client_request_t *, http_client_response_t *);
int http_client_free(http_client_response_t *);
#endif
