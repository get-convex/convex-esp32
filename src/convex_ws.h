#pragma once
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

/* Internal WebSocket-over-TLS transport for the Convex client, built on
 * WiFiClientSecure -- which ships with every ESP32 Arduino core, so the library
 * needs no external component and installs cleanly in the Arduino IDE and
 * PlatformIO alike. The framing (handshake, client masking, ping/pong,
 * continuation reassembly) mirrors the implementation validated against a live
 * Convex deployment. Not a public API; use Convex.h.
 *
 * One background task owns the socket: it reads frames and drains a send queue,
 * so reads and writes never race on the TLS object. */

// A complete or partial inbound message. `offset`/`total` frame reassembly the
// same way esp_websocket_client's DATA event does, so the sync layer's existing
// logic is unchanged. op_code: 0x1 text, 0x2 binary, 0x0 continuation.
typedef void (*CxWsOnData)(uint8_t opcode, const char *data, int len, int offset, int total);
typedef void (*CxWsOnState)(bool connected);

// `wssUri` is wss://host[:port]/path. Spawns the task; connects asynchronously
// with automatic reconnect. Delivers via the callbacks (from the task thread).
bool cxwsBegin(const char *wssUri, CxWsOnData onData, CxWsOnState onState,
               int rxChunk);
void cxwsEnd();
bool cxwsConnected();
int  cxwsSendText(const char *data, size_t len);   // queued; returns len or -1
void cxwsPause();    // disconnect and stop reconnecting (frees the TLS session)
void cxwsResume();   // reconnect

// Optional: supply a CA bundle for TLS verification (DER x509 bundle, as the
// ESP32 core embeds). If never called, the transport verifies against the core's
// built-in bundle; pass nullptr to disable verification (development only).
void cxwsSetCACertBundle(const uint8_t *bundle, size_t size);
