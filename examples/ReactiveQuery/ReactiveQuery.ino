// Convex ESP32 — reactive query example (ALPHA)
//
// Subscribes to a Convex query and prints its value every time it changes on
// the server, with no polling. Fill in your WiFi and deployment below.
//
// Requires: ArduinoJson, and the esp_websocket_client component. With
// PlatformIO see this folder's platformio.ini. On the Arduino IDE, install
// ArduinoJson from the Library Manager (esp_websocket_client ships with the
// ESP32 core).

#include <WiFi.h>
#include <Convex.h>

static const char *WIFI_SSID = "your-ssid";
static const char *WIFI_PASS = "your-password";

// Your deployment's Cloud URL (Settings → URL, or `npx convex dev` output).
static const char *CONVEX_URL = "https://your-deployment.convex.cloud";

// A query you have defined, as "file:function", plus its args.
static const char *QUERY = "messages:list";

// Fires on subscribe and on every change. `value` is valid only in here.
void onQuery(int queryId, bool ok, JsonVariantConst value, void *user) {
  if (!ok) { Serial.printf("query %d failed on server\n", queryId); return; }
  String json;
  serializeJson(value, json);
  Serial.printf("query %d -> %s\n", queryId, json.c_str());
}

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("wifi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.printf(" connected, ip %s\n", WiFi.localIP().toString().c_str());

  convexEnableTelemetry(true);          // optional: emit a ClientConnect event
  if (!convexBegin(CONVEX_URL)) {
    Serial.printf("convexBegin failed: %s\n", convexLastError());
    return;
  }
  // convexSetAuth("<jwt>");            // optional, for authenticated queries

  JsonDocument args;                    // add fields if your query takes args
  int q = convexSubscribe(QUERY, args, onQuery, nullptr);
  Serial.printf("subscribed %s -> query %d\n", QUERY, q);
}

void loop() {
  static bool wasConnected = false;
  bool now = convexConnected();
  if (now != wasConnected) {
    Serial.printf("convex %s\n", now ? "connected" : "disconnected");
    wasConnected = now;
  }
  delay(500);
}
