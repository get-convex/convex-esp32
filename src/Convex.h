#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include "convex_config.h"

/* Convex client for the ESP32  --  ALPHA, no guarantee of support.
 * https://github.com/get-convex/convex-esp32
 *
 * Talks to a Convex deployment two ways:
 *   1. The reactive sync protocol over a TLS WebSocket (wss://<dep>.convex.cloud
 *      /api/<ver>/sync): reactive query subscriptions, plus mutations and
 *      actions. Subscribe once and the server pushes a new value whenever the
 *      query result changes -- no polling.
 *   2. HTTP actions (routes you define with httpAction on <dep>.convex.site),
 *      via convexHttpAction().
 *
 * Transport is esp_websocket_client / esp_http_client with the ESP-IDF
 * certificate bundle (esp_crt_bundle), so TLS validates against real roots and
 * no certificates are pinned in your firmware. Requires ArduinoJson.
 *
 * THREADING: the socket runs its own background task. There are two ways to
 * consume a query: (a) a callback, which fires from that task -- keep it short,
 * copy what you keep, never block; or (b) subscribe without a callback and poll
 * the cached value from your own loop() with convexQueryChanged/convexQueryValue,
 * which never crosses threads in your code. The convex* calls are safe from any
 * task.
 *
 * STATUS: alpha. The API may change. Provided as-is, without warranty or
 * committed support (Apache-2.0). Issues and PRs welcome, best-effort.
 */

// ---- reactive sync (WebSocket) --------------------------------------------

/* A reactive query result. `value` is the current query value straight off the
 * wire (object, array, scalar, or null) and is valid only during the callback
 * -- copy anything you keep. `ok` is false when the query threw on the server. */
typedef void (*ConvexQueryCb)(int queryId, bool ok, JsonVariantConst value, void *user);

/* A one-shot mutation/action result, valid only during the callback. */
typedef void (*ConvexResultCb)(int requestId, bool ok, JsonVariantConst result, void *user);

/* Notified on every connect/disconnect edge. */
typedef void (*ConvexStateCb)(bool connected, void *user);

/* Lambda-friendly callback forms (can capture). A small heap cost per use. */
typedef std::function<void(bool ok, JsonVariantConst value)> ConvexQueryFn;
typedef std::function<void(bool ok, JsonVariantConst result)> ConvexResultFn;

/* Start the client and its background task. `cloudUrl` is the deployment's
 * https://<name>.convex.cloud (NOT .site). Connecting is asynchronous; watch
 * convexConnected(). Returns false only if the client could not be created. */
bool convexBegin(const char *cloudUrl);
void convexEnd();
bool convexConnected();

/* Set (nullptr clears) the auth token sent to Convex as a "User" identity. It
 * takes effect on the next (re)connect and is re-sent after every reconnect. */
void convexSetAuth(const char *token);

/* One state handler; nullptr clears. */
void convexOnState(ConvexStateCb cb, void *user);

/* Subscribe to a reactive query. `udfPath` is "file:function"; `args` is the
 * single Convex args object (may be empty). Returns a queryId (>=0), or -1 if
 * the subscription table is full. Args are copied.
 *
 * Three forms:
 *   - callback (raw fn + user pointer): fires from the socket task on each change.
 *   - callback (std::function): same, but can capture. Small heap cost.
 *   - no callback: nothing fires; instead the latest value is cached and you
 *     read it from loop() with convexQueryChanged()/convexQueryValue(). This is
 *     the thread-safe, Arduino-friendly path -- and the only one that keeps a
 *     cached copy (a callback subscription does not, to save memory). */
int  convexSubscribe(const char *udfPath, const JsonDocument &args,
                     ConvexQueryCb cb, void *user);
int  convexSubscribe(const char *udfPath, const JsonDocument &args,
                     ConvexQueryFn cb);
int  convexSubscribe(const char *udfPath, const JsonDocument &args);
void convexUnsubscribe(int queryId);

/* Poll a cached (no-callback) subscription from your own loop(). Returns true
 * once per change; reading clears the flag. convexQueryValue parses the latest
 * cached value into `out` (returns false if there is none yet or it did not
 * fit CONVEX_RX_MAX). Both are no-ops for a callback subscription. */
bool convexQueryChanged(int queryId);
bool convexQueryValue(int queryId, JsonDocument &out);

/* Run a mutation or action over the socket. `cb` (may be null) fires once with
 * the result. Returns a requestId (>=0) or -1 if offline / table full. */
int  convexMutation(const char *udfPath, const JsonDocument &args,
                    ConvexResultCb cb, void *user);
int  convexAction(const char *udfPath, const JsonDocument &args,
                  ConvexResultCb cb, void *user);
int  convexMutation(const char *udfPath, const JsonDocument &args, ConvexResultFn cb);
int  convexAction(const char *udfPath, const JsonDocument &args, ConvexResultFn cb);

/* Release the socket's TLS session (e.g. to free heap for an HTTP upload) and
 * later reconnect, replaying every subscription and the auth token. */
void convexPause();
void convexResume();

const char *convexLastError();
int      convexSubCount();
uint32_t convexBytesIn();
uint32_t convexBytesOut();
/* Lowest free internal heap seen since boot (bytes) -- watch memory tightness. */
uint32_t convexHeapLowWater();

// ---- telemetry ------------------------------------------------------------

/* Report a client telemetry event to Convex over the socket, as the browser
 * client does: it sends {type:"Event", eventType, event}. `event` is any JSON
 * object. No-op when not connected. Use it to surface device health, timings,
 * or custom counters to your deployment. */
void convexReportEvent(const char *eventType, const JsonDocument &event);

/* When enabled, the client auto-emits a "ClientConnect" telemetry event on each
 * (re)connect: {connectionCount, freeHeap, largestBlock, connectMs}. Off by
 * default. Mirrors the browser client's reportMarks(). */
void convexEnableTelemetry(bool on);

// ---- HTTP actions (<deployment>.convex.site) ------------------------------

/* Call an httpAction route. `method` is "GET"/"POST"/"PUT"/"PATCH"/"DELETE".
 * `pathOrUrl` is an absolute https URL, or a path ("/foo") resolved against the
 * deployment's .convex.site door (derived from the URL passed to convexBegin();
 * or set it explicitly with convexSetHttpBase). `body` may be null. When
 * `token` is non-null it is sent as "Authorization: Bearer <token>".
 * `contentType` defaults to application/json when a body is present. Fills
 * `resp` with the response body (truncated to cap) and returns the HTTP status
 * code, or a negative value on a transport error. Blocking: call it off any
 * time-critical loop.
 *
 * MEMORY: an HTTP action needs its own TLS session. On boards where only one
 * session fits, running it while the sync socket is up would OOM, so by default
 * this pauses the socket for the duration and resumes it after (subscriptions
 * replay on reconnect). Pass keepSocket=true to skip that if you know two
 * sessions fit. */
int convexHttpAction(const char *method, const char *pathOrUrl, const char *body,
                     const char *token, char *resp, size_t respCap,
                     const char *contentType = nullptr, bool keepSocket = false);

/* Override the base used to resolve relative HTTP-action paths. Normally derived
 * automatically from convexBegin() (.convex.cloud -> .convex.site). */
void convexSetHttpBase(const char *siteUrl);
