# Convex for embedded — ESP32 (**alpha**)

A [Convex](https://convex.dev) client for the ESP32, in C++. It speaks the same
sync protocol the browser client does, so **queries are reactive**: subscribe
once and the server pushes a new value whenever the result changes — no polling.
Mutations and actions ride the same TLS WebSocket, and HTTP actions are one call
away.

It is **self-contained**: the WebSocket runs over `WiFiClientSecure`, which ships
with every ESP32 Arduino core, so there is no external component to install and
it works in both the Arduino IDE and PlatformIO with only ArduinoJson.

> [!WARNING]
> **This is alpha software with no guarantee of support.** The API may change
> without notice. It is provided as-is, without warranty (Apache-2.0). Issues
> and PRs are welcome and handled best-effort. Don't build anything you can't
> afford to have break.

## Why

The usual way to get backend data onto a microcontroller is to poll an HTTP
endpoint on a timer: slow, wasteful of the radio and battery, and always a
little stale. This library instead holds a Convex sync socket open, so your
device is pushed the new value the moment a query's result changes — the same
way a Convex web app updates.

## What works

- **Reactive query subscriptions** — `convexSubscribe`, with a callback on every change.
- **Mutations and actions** over the socket — `convexMutation`, `convexAction`.
- **Auth** — `convexSetAuth` (sent as a Convex `User` identity), re-sent on reconnect.
- **Automatic reconnect** with full query-set + auth + in-flight-request replay.
- **HTTP actions** — `convexHttpAction` for the routes you define with `httpAction`.
- **Telemetry** — `convexReportEvent` / `convexEnableTelemetry`, using the
  protocol's `Event` message (as the browser client does).
- TLS via the ESP32 core's built-in **root CA bundle** — real cert validation,
  nothing to pin or rotate.

### Not (yet) handled

`TransitionChunk` reassembly (only emitted for very large query results),
pagination journals, and optimistic updates. Query/request tables are small and
fixed-size (12 subscriptions, 8 in-flight requests). PRs welcome.

## Requirements

- An ESP32 (PSRAM recommended — JSON and the receive buffer prefer it, and fall
  back to internal RAM).
- Arduino-ESP32 core **3.2 or newer** (for `WiFiClientSecure::setCACertBundle`).
- [ArduinoJson](https://arduinojson.org) 7.x.

No WebSocket component or extra transport is required — `WiFiClientSecure` and
`esp_http_client` both come with the core.

## Install

### PlatformIO

```ini
[env:esp32s3]
platform  = espressif32
board     = esp32-s3-devkitc-1
framework = arduino
lib_deps  =
    https://github.com/get-convex/convex-esp32.git
    bblanchon/ArduinoJson@^7.0.0
build_flags = -DARDUINOJSON_ENABLE_ARDUINO_STRING=1
```

### Arduino IDE

Install **ArduinoJson** from the Library Manager, then add this library
(*Sketch → Include Library → Add .ZIP Library*, or clone into your `libraries`
folder). Open *File → Examples → Convex → ReactiveQuery*.

## Usage

```cpp
#include <WiFi.h>
#include <Convex.h>

void onMessages(int queryId, bool ok, JsonVariantConst value, void *user) {
  if (!ok) return;
  String json; serializeJson(value, json);
  Serial.printf("messages -> %s\n", json.c_str());   // fires on every change
}

void setup() {
  Serial.begin(115200);
  WiFi.begin("ssid", "password");
  while (WiFi.status() != WL_CONNECTED) delay(200);

  convexBegin("https://your-deployment.convex.cloud");   // NOT .site
  // convexSetAuth("<jwt>");                              // optional

  JsonDocument args;                                     // args["channel"] = "general";
  convexSubscribe("messages:list", args, onMessages, nullptr);
}

void loop() { delay(1000); }
```

Mutations and actions:

```cpp
void onSent(int reqId, bool ok, JsonVariantConst result, void *user) {
  Serial.printf("send %s\n", ok ? "ok" : "failed");
}
JsonDocument a; a["body"] = "hello from an ESP32";
convexMutation("messages:send", a, onSent, nullptr);
```

An HTTP action:

```cpp
char resp[512];
int status = convexHttpAction("POST", "/webhook", "{\"ping\":true}",
                              /*token=*/nullptr, resp, sizeof(resp));
Serial.printf("http %d: %s\n", status, resp);
```

Telemetry (uses the protocol's `Event` message):

```cpp
convexEnableTelemetry(true);        // auto-emit a ClientConnect event on connect
JsonDocument ev; ev["battery"] = 84; ev["fw"] = "1.4.0";
convexReportEvent("DeviceHealth", ev);
```

See [`examples/ReactiveQuery`](examples/ReactiveQuery) for a complete sketch.

## API

| Function | What it does |
| --- | --- |
| `convexBegin(cloudUrl)` | Start the client + background task. `https://<dep>.convex.cloud`. |
| `convexEnd()` | Stop and tear down. |
| `convexConnected()` | Is the socket up. |
| `convexSetAuth(token)` | Set/clear the `User` auth token. |
| `convexOnState(cb, user)` | Connect/disconnect callback. |
| `convexOnAuthError(cb, user)` | Fires when the server rejects the token — re-mint and `convexSetAuth()`. |
| `convexSubscribe(udfPath[, args][, cb[, user]][, cache])` | Reactive query; returns a queryId. Cached by default; optional push callback (lambda or C fn+user); `cache=false` for push-only. |
| `convexQueryChanged(queryId)` / `convexQueryValue(queryId, out)` | Poll a subscription's latest value. |
| `convexQueryOnce(udfPath[, args], cb)` | One-shot query: first value, then auto-unsubscribe. |
| `convexUnsubscribe(queryId)` | Drop a subscription. |
| `convexMutation/Action(udfPath[, args][, cb])` | Run it. Args as JsonDocument, JSON string, or omitted; C or `std::function` callback. |
| `convexPause()` / `convexResume()` | Release / re-establish the socket (e.g. to free TLS heap). |
| `convexHttpAction(method, pathOrUrl, body, token, resp, cap[, contentType, keepSocket])` | Call an `httpAction` route. |
| `convexReportEvent(eventType, event)` | Send a telemetry `Event`. |
| `convexEnableTelemetry(on)` | Auto-emit `ClientConnect` on connect. |
| `convexLastStatus()` / `convexStatusStr(s)` / `convexLastError()` | Machine + human error of the last failed call (negative returns are a `ConvexStatus`). |
| `convexSubCount()`, `convexBytesIn/Out()`, `convexHeapLowWater()` | Diagnostics. |

### Reading a query: poll, callback, or both

A subscription is **cached by default**, so the simplest, thread-safe path is to
poll it from your own `loop()` — nothing runs on the socket task:

```cpp
int q = convexSubscribe("messages:list");          // cached; poll it
// in loop():
if (convexQueryChanged(q)) {
  JsonDocument doc;
  if (convexQueryValue(q, doc)) { /* newest value, on your thread */ }
}
```

Add a **callback** to also get pushes (it fires from the socket task — keep it
short, copy what you keep, never block):

```cpp
convexSubscribe("messages:list", args, [](bool ok, JsonVariantConst v) { ... });
```

Push-only and memory-tight? Pass `cache=false` to skip the cached copy:

```cpp
convexSubscribe("messages:list", args, onMsg, /*cache=*/false);
```

## Memory & tuning

The ESP32 killers are the TLS session (~40 KB peak), unbounded JSON, and
fragmentation. This library is built to bound all three, and to degrade instead
of crash:

- **One TLS session.** The socket holds one open. `convexHttpAction` needs its
  own, so by default it **pauses the socket** for the call and resumes after
  (pass `keepSocket=true` to skip). `convexPause()/Resume()` do the same around
  your own HTTPS. A live sync socket can often *replace* your polling outright.
- **Bounded, fail-soft buffers.** A message over `CONVEX_RX_MAX` is dropped with
  an error, never grown; the send queue is capped at `CONVEX_SEND_QUEUE_MAX`.
- **Low-heap guard.** `convexBegin` refuses to open TLS below `CONVEX_MIN_HEAP`,
  returning an error instead of crashing mid-handshake. Watch
  `convexHeapLowWater()`.
- **Compile-time knobs** (override with `-D`): `CONVEX_MAX_SUBS` (12),
  `CONVEX_MAX_REQS` (8), `CONVEX_ARGS_BUF` (320), `CONVEX_RX_MAX`
  (48 KB with PSRAM, 8 KB without), `CONVEX_MIN_HEAP` (45000),
  `CONVEX_SEND_QUEUE_MAX` (16).
- **Keep payloads small.** Subscribe to *narrow* queries and let the server do
  the shaping — a projected query or an `httpAction` returning a minimal (even
  packed/base64) body beats materializing a big document on the device.

### Shrink the TLS record buffers (biggest single win)

mbedTLS defaults to 16 KB in + 16 KB out per session; Convex sync frames are
small, so on an ESP-IDF build you can reclaim ~25 KB with:

```
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048
```

## Robustness

- **Reconnects** automatically and, on each reconnect, re-sends `Connect` (with
  the last observed timestamp), re-authenticates, re-declares the whole query
  set, and replays in-flight requests. The stable `sessionId` + replayed
  `requestId` let the server dedup a mutation interrupted by a drop, so it is
  not applied twice.
- **Bounded against a hostile/broken server:** an oversized message drops the
  connection instead of growing the heap; the send queue is capped; a request
  whose reply never arrives is failed after `CONVEX_REQ_TIMEOUT_MS` so the table
  can't fill; malformed JSON and frames are rejected, never trusted.
- **Fuzzed.** The JSON message handler and the WebSocket frame parser have each
  been run through 500k iterations of mutated and random input under
  AddressSanitizer + UndefinedBehaviorSanitizer with no crash, memory error, or
  UB. Still alpha — this is not a security audit.

## TLS

By default the client validates the server against the ESP32 core's embedded
root CA bundle. To use your own bundle, or (for development only) to disable
verification, see `cxwsSetCACertBundle` in `src/convex_ws.h`.

## License

Apache-2.0 © Convex, Inc.
