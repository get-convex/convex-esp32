# Convex for ESP32 — **alpha**

A [Convex](https://convex.dev) client for the ESP32, in C++. It speaks the same
sync protocol the browser client does, so **queries are reactive**: subscribe
once and the server pushes a new value whenever the result changes — no polling.
Mutations and actions ride the same TLS WebSocket, and HTTP actions are one call
away.

> [!WARNING]
> **This is alpha software with no guarantee of support.** The API may change
> without notice. It is provided as-is, without warranty (Apache-2.0). Issues
> and PRs are welcome and handled best-effort. Do not build anything you can't
> afford to have break.

## Why

The usual way to get backend data onto a microcontroller is to poll an HTTP
endpoint on a timer. That is slow, wasteful of the radio and battery, and always
a little stale. This library instead holds a Convex sync socket open: your device
gets pushed the new value the moment a query's result changes, the same way a
Convex web app does.

## What works

- **Reactive query subscriptions** — `convexSubscribe`, with a callback on every change.
- **Mutations and actions** over the socket — `convexMutation`, `convexAction`.
- **Auth** — `convexSetAuth` (sent as a Convex `User` identity), re-sent on reconnect.
- **Automatic reconnect** with full query-set + auth + in-flight-request replay.
- **HTTP actions** — `convexHttpAction` for the routes you define with `httpAction`.
- **Telemetry** — `convexReportEvent` / `convexEnableTelemetry`, using the
  protocol's `Event` message (as the browser client does).
- TLS via the ESP-IDF **certificate bundle** — validates against real roots, no
  pinned certs to rotate.

### Not (yet) handled

`TransitionChunk` reassembly (only emitted for very large query results),
pagination journals, and optimistic updates. Query/request tables are small,
fixed-size (12 subscriptions, 8 in-flight requests). PRs welcome.

## Requirements

- An ESP32 with PSRAM recommended (JSON and the receive buffer prefer it; it
  falls back to internal RAM).
- [ArduinoJson](https://arduinojson.org) 7.x.
- The `esp_websocket_client` ESP-IDF component (for the WebSocket) and
  `esp_http_client` (in the core, for HTTP actions).

## Install

### PlatformIO

```ini
[env:esp32s3]
platform  = espressif32
board     = esp32-s3-devkitc-1
framework = arduino, espidf          ; the esp-idf combo lets the component load
lib_deps  =
    https://github.com/get-convex/esp32.git
    bblanchon/ArduinoJson@^7.0.0
custom_component_add =
    espressif/esp_websocket_client@^1.2.0
build_flags = -DARDUINOJSON_ENABLE_ARDUINO_STRING=1
```

`esp_websocket_client` is not part of the Arduino closure, so it is pulled as an
ESP-IDF managed component — which is why the example uses `framework = arduino,
espidf`. If you build with a pure `framework = arduino`, add the component some
other way (e.g. an `idf_component.yml`).

### ESP-IDF

Add this repo as a component (it ships an `idf_component.yml` that declares
`esp_websocket_client`), and provide ArduinoJson.

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

  JsonDocument args;                                     // args.["channel"] = "general";
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
| `convexSubscribe(udfPath, args, cb, user)` | Reactive query; returns a queryId. |
| `convexUnsubscribe(queryId)` | Drop a subscription. |
| `convexMutation(udfPath, args, cb, user)` | Run a mutation; returns a requestId. |
| `convexAction(udfPath, args, cb, user)` | Run an action; returns a requestId. |
| `convexPause()` / `convexResume()` | Release / re-establish the socket (e.g. to free TLS heap). |
| `convexHttpAction(method, pathOrUrl, body, token, resp, cap[, contentType])` | Call an `httpAction` route. |
| `convexReportEvent(eventType, event)` | Send a telemetry `Event`. |
| `convexEnableTelemetry(on)` | Auto-emit `ClientConnect` on connect. |
| `convexLastError()`, `convexSubCount()`, `convexBytesIn/Out()` | Diagnostics. |

Callbacks fire from the socket's background task — keep them short, copy what you
keep, and never block.

## A note on heap

The socket holds one TLS session open for its lifetime. On parts where only one
TLS session fits at a time, coordinate with any other HTTPS you do:
`convexPause()` releases the session for a large upload, and `convexResume()`
reconnects and replays every subscription. A persistent sync socket can often
*replace* your polling and HTTP calls outright, which is a net win.

## License

Apache-2.0 © Convex, Inc.
