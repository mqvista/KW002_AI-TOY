/**
 * @file xz_mcp.h
 * @brief Device side MCP server (JSON-RPC 2.0 over the XiaoZhi "mcp" message).
 */

#ifndef __XZ_MCP_H__
#define __XZ_MCP_H__

#include "tuya_cloud_types.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Sends one JSON-RPC message; the caller wraps it as {"type":"mcp","payload":...}. */
typedef void (*XZ_MCP_SEND_CB)(const char *payload);

/** Handle the payload of a server "mcp" message (runs the tool in the caller's thread). */
void xz_mcp_handle(const cJSON *payload, XZ_MCP_SEND_CB send);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_MCP_H__ */
