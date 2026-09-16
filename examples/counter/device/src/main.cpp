// Convex ESP32 demo — ESP32-S3-Touch-LCD-1.54.
// Both the device and the web page connect to a tiny Convex deployment holding a
// shared counter (see ../cxdemo-backend). Tap the screen (or press BOOT) and the
// count goes up on the device AND the web, instantly — and vice-versa. Fully
// reactive over a WebSocket push (no polling), via the convex-esp32 library.
#include <WiFi.h>
#include <Convex.h>
#include <esp_random.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>

// ---- config ----
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
// Set to your demo deployment (cxdemo-backend `npx convex dev` prints it).
static const char *CONVEX_URL = "https://YOUR-DEPLOYMENT.convex.cloud";

// ---- ESP32-S3-Touch-LCD-1.54 pins (ST7789 240x240), from Waveshare's example ----
#define LCD_DC 45
#define LCD_CS 21
#define LCD_SCK 38
#define LCD_MOSI 39
#define LCD_RST 40
#define LCD_BL 46
#define BTN_BOOT 0
#define TP_SDA 42
#define TP_SCL 41
#define TP_INT 48
#define TP_RST 47

#define BLACK 0x0000
#define WHITE 0xFFFF
#define CYAN  0x07FF
#define GREEN 0x07E0
#define YELLOW 0xFFE0
#define RED   0xF800

static Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI, -1);
static Arduino_GFX *gfx = new Arduino_ST7789(bus, LCD_RST, 0, true, 240, 240);
static TouchDrvCSTXXX touch;
static bool gTouchOk = false;

// ---- state (mirror of the shared Convex counter) ----
static int  gQ = -1;
static long gCount = 0;
static char gLastActor[24] = "-";
static bool gDirty = true, gWifiOk = false, gConnOk = false;

static void tap(const char *src) {
  Serial.printf("[demo] %s -> tap\n", src);
  convexMutation("demo:tap", "{\"who\":\"esp32\"}", nullptr);   // bump the shared counter
}

static void draw() {
  gfx->fillScreen(BLACK);
  gfx->setTextColor(CYAN); gfx->setTextSize(2);
  gfx->setCursor(8, 8); gfx->print("Convex  ESP32");
  gfx->drawFastHLine(0, 32, 240, 0x39C7);

  // big shared count
  gfx->setTextColor(WHITE); gfx->setTextSize(9);
  char n[12]; snprintf(n, sizeof(n), "%ld", gCount);
  int w = strlen(n) * 6 * 9;
  gfx->setCursor((240 - w) / 2, 66); gfx->print(n);

  // last actor
  gfx->setTextSize(2); gfx->setTextColor(0xAD55);
  char last[40]; snprintf(last, sizeof(last), "last: %s", gLastActor);
  int lw = strlen(last) * 6 * 2;
  gfx->setCursor((240 - lw) / 2, 150); gfx->print(last);

  // hint + status
  gfx->setTextSize(2); gfx->setTextColor(gConnOk ? GREEN : YELLOW);
  gfx->setCursor(40, 196); gfx->print(gConnOk ? "TAP TO +1" : "connecting");
  gfx->setTextSize(1); gfx->setTextColor(gConnOk ? GREEN : (gWifiOk ? YELLOW : RED));
  gfx->setCursor(8, 228);
  gfx->printf("%s   rssi %d", gConnOk ? "convex connected" : (gWifiOk ? "wifi ok" : "no wifi"), (int)WiFi.RSSI());
}

static void refreshState() {
  if (gQ < 0 || !convexQueryChanged(gQ)) return;
  JsonDocument doc;
  if (!convexQueryValue(gQ, doc)) return;
  gCount = doc["count"] | 0L;
  snprintf(gLastActor, sizeof(gLastActor), "%s", doc["lastActor"] | "-");
  Serial.printf("[demo] count=%ld last=%s\n", gCount, gLastActor);
  gDirty = true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Convex ESP32 counter demo ===");
  pinMode(BTN_BOOT, INPUT_PULLUP);
  pinMode(LCD_BL, OUTPUT); digitalWrite(LCD_BL, HIGH);
  gfx->begin(); gfx->fillScreen(BLACK);
  touch.setPins(TP_RST, TP_INT);
  gTouchOk = touch.begin(Wire, CST816_SLAVE_ADDRESS, TP_SDA, TP_SCL);
  Serial.printf("touch %s\n", gTouchOk ? "ok" : "FAILED");
  gfx->setTextColor(CYAN); gfx->setTextSize(2);
  gfx->setCursor(8, 100); gfx->print("Convex ESP32");
  gfx->setCursor(8, 130); gfx->setTextColor(WHITE); gfx->print("joining wifi...");

  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) { delay(250); Serial.print("."); }
  gWifiOk = WiFi.status() == WL_CONNECTED;
  Serial.printf("\nwifi %s ip %s\n", gWifiOk ? "ok" : "FAILED", WiFi.localIP().toString().c_str());

  convexOnState([](bool up, void *) { gConnOk = up; gDirty = true; }, nullptr);
  if (!convexBegin(CONVEX_URL)) Serial.printf("convexBegin failed: %s\n", convexLastError());
}

void loop() {
  static bool subd = false;
  static uint32_t lastBtn = 0;
  static int lastBtnState = HIGH;

  if (!subd && convexConnected()) { subd = true; gQ = convexSubscribe("demo:get"); Serial.printf("subscribed demo:get -> q%d\n", gQ); }

  int b = digitalRead(BTN_BOOT);
  if (b == LOW && lastBtnState == HIGH && millis() - lastBtn > 40) { lastBtn = millis(); tap("BOOT"); }
  lastBtnState = b;

  static bool wasTouched = false;
  if (gTouchOk) {
    int16_t tx = 0, ty = 0;
    bool now = touch.getPoint(&tx, &ty, 1) > 0;
    if (now && !wasTouched && millis() - lastBtn > 40) { lastBtn = millis(); tap("TAP"); }
    wasTouched = now;
  }

  refreshState();
  if (gDirty) { gDirty = false; draw(); }
  delay(20);
}
