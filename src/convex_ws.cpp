#include "convex_ws.h"
#include "convex_config.h"
#include <WiFiClientSecure.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string>
#include <deque>

// The ESP32 Arduino core embeds a root CA bundle; use it by default.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

static WiFiClientSecure gTls;
static char gHost[80] = "", gPath[96] = "";
static int  gPort = 443;
static int  gRxChunk = 4096;
static CxWsOnData  gOnData = nullptr;
static CxWsOnState gOnState = nullptr;

static TaskHandle_t gTask = nullptr;
static volatile bool gRunning = false;
static volatile bool gPaused = false;
static volatile bool gConnected = false;

static SemaphoreHandle_t gSendMx = nullptr;
static std::deque<std::string> gSendQ;

static const uint8_t *gBundle = nullptr;
static size_t gBundleSize = 0;
static bool gBundleSet = false;

void cxwsSetCACertBundle(const uint8_t *bundle, size_t size) {
  gBundle = bundle; gBundleSize = size; gBundleSet = true;
}
bool cxwsConnected() { return gConnected; }

// ---- tiny base64 ----
static std::string b64(const unsigned char *p, size_t n) {
  static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = p[i] << 16;
    if (i + 1 < n) v |= p[i + 1] << 8;
    if (i + 2 < n) v |= p[i + 2];
    o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63];
    o += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
    o += (i + 2 < n) ? T[v & 63] : '=';
  }
  return o;
}

static void applyTls() {
  if (!gBundleSet)
    gTls.setCACertBundle(rootca_crt_bundle_start,
                         (size_t)(rootca_crt_bundle_end - rootca_crt_bundle_start));
  else if (gBundle)       gTls.setCACertBundle(gBundle, gBundleSize);
  else                    gTls.setInsecure();   // development only
}

static bool wsHandshake() {
  unsigned char key[16]; esp_fill_random(key, sizeof(key));
  std::string k = b64(key, 16);
  char req[420];
  int n = snprintf(req, sizeof(req),
    "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: https://%s\r\n\r\n",
    gPath, gHost, k.c_str(), gHost);
  if (gTls.write((const uint8_t *)req, n) != (size_t)n) return false;

  std::string resp;
  uint32_t deadline = millis() + 8000;
  while (resp.find("\r\n\r\n") == std::string::npos) {
    if (millis() > deadline || !gTls.connected()) return false;
    while (gTls.available()) { int c = gTls.read(); if (c >= 0) resp += (char)c; }
    if (resp.size() > 8192) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return resp.find(" 101 ") != std::string::npos;
}

static void queueFrame(uint8_t opcode, const char *p, size_t n) {
  std::string f;
  f += (char)(0x80 | opcode);
  unsigned char mask[4]; esp_fill_random(mask, 4);
  if (n < 126) f += (char)(0x80 | n);
  else if (n < 65536) { f += (char)(0x80 | 126); f += (char)(n >> 8); f += (char)(n & 0xff); }
  else { f += (char)(0x80 | 127); for (int j = 7; j >= 0; --j) f += (char)((n >> (j * 8)) & 0xff); }
  f.append((char *)mask, 4);
  size_t base = f.size();
  f.append(p, n);
  for (size_t j = 0; j < n; ++j) f[base + j] ^= mask[j % 4];
  xSemaphoreTake(gSendMx, portMAX_DELAY);
  // Bound the queue: if the socket has stalled, drop the oldest rather than let
  // the backlog grow the heap without limit.
  while (gSendQ.size() >= (size_t)CONVEX_SEND_QUEUE_MAX) gSendQ.pop_front();
  gSendQ.push_back(std::move(f));
  xSemaphoreGive(gSendMx);
}

int cxwsSendText(const char *data, size_t len) {
  if (!gConnected) return -1;
  queueFrame(0x1, data, len);
  return (int)len;
}

// Deliver a complete message, chunked by rxChunk to exercise the sync layer's
// payload_offset reassembly the same way the IDF client would.
static void deliver(uint8_t opcode, const std::string &msg) {
  int total = (int)msg.size();
  int chunk = gRxChunk > 0 ? gRxChunk : 4096;
  for (int off = 0; off < total; off += chunk) {
    int len = total - off < chunk ? total - off : chunk;
    if (gOnData) gOnData(opcode, msg.data() + off, len, off, total);
  }
}

static void parseFrames(std::string &in, uint8_t &msgOp, std::string &msg) {
  size_t i = 0;
  for (;;) {
    if (in.size() - i < 2) break;
    uint8_t b0 = in[i], b1 = in[i + 1];
    bool fin = b0 & 0x80; uint8_t op = b0 & 0x0f; bool masked = b1 & 0x80;
    uint64_t len = b1 & 0x7f; size_t hdr = 2;
    if (len == 126) { if (in.size() - i < 4) break; len = ((uint8_t)in[i+2] << 8) | (uint8_t)in[i+3]; hdr = 4; }
    else if (len == 127) {
      if (in.size() - i < 10) break; len = 0;
      for (int j = 0; j < 8; ++j) len = (len << 8) | (uint8_t)in[i + 2 + j];
      hdr = 10;
    }
    size_t maskLen = masked ? 4 : 0;
    if (in.size() - i < hdr + maskLen + (size_t)len) break;   // incomplete
    std::string payload(in.data() + i + hdr + maskLen, len);
    if (masked) { const unsigned char *m = (const unsigned char *)(in.data() + i + hdr);
      for (size_t j = 0; j < payload.size(); ++j) payload[j] ^= m[j % 4]; }
    i += hdr + maskLen + len;

    if (op == 0x9) { queueFrame(0xA, payload.data(), payload.size()); continue; } // ping->pong
    if (op == 0x8) { gConnected = false; continue; }                             // close
    if (op == 0xA) continue;                                                     // pong
    if (op != 0x0) { msgOp = op; msg = payload; } else msg += payload;
    if (fin) { deliver(msgOp, msg); msg.clear(); }
  }
  if (i) in.erase(0, i);
}

static bool connectOnce() {
  applyTls();
  gTls.setHandshakeTimeout(15);
  if (!gTls.connect(gHost, gPort)) return false;
  return wsHandshake();
}

static void serviceLoop() {
  std::string in; uint8_t msgOp = 0x1; std::string msg;
  uint8_t buf[1536];
  while (gRunning && !gPaused && gTls.connected() && gConnected) {
    bool did = false;
    int avail = gTls.available();
    if (avail > 0) {
      int r = gTls.read(buf, sizeof(buf));
      if (r > 0) { in.append((char *)buf, r); did = true; }
      parseFrames(in, msgOp, msg);
    }
    std::string f;
    for (;;) {
      xSemaphoreTake(gSendMx, portMAX_DELAY);
      if (gSendQ.empty()) { xSemaphoreGive(gSendMx); break; }
      f = std::move(gSendQ.front()); gSendQ.pop_front();
      xSemaphoreGive(gSendMx);
      gTls.write((const uint8_t *)f.data(), f.size());
      did = true;
    }
    if (!did) vTaskDelay(pdMS_TO_TICKS(15));
  }
}

static void runTask(void *) {
  for (;;) {
    if (!gRunning) break;
    if (gPaused) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    if (connectOnce()) {
      gConnected = true;
      if (gOnState) gOnState(true);
      serviceLoop();
      gConnected = false;
      if (gOnState) gOnState(false);
    }
    gTls.stop();
    { xSemaphoreTake(gSendMx, portMAX_DELAY); gSendQ.clear(); xSemaphoreGive(gSendMx); }
    for (int s = 0; gRunning && !gPaused && s < 5000; s += 100) vTaskDelay(pdMS_TO_TICKS(100));
  }
  gTask = nullptr;
  vTaskDelete(nullptr);
}

static void parseUri(const char *uri) {
  const char *p = strstr(uri, "://");
  const char *rest = p ? p + 3 : uri;
  const char *slash = strchr(rest, '/');
  snprintf(gPath, sizeof(gPath), "%s", slash ? slash : "/");
  char hostport[96];
  size_t hl = slash ? (size_t)(slash - rest) : strlen(rest);
  if (hl >= sizeof(hostport)) hl = sizeof(hostport) - 1;
  memcpy(hostport, rest, hl); hostport[hl] = 0;
  char *colon = strchr(hostport, ':');
  if (colon) { *colon = 0; gPort = atoi(colon + 1); } else gPort = 443;
  snprintf(gHost, sizeof(gHost), "%s", hostport);
}

bool cxwsBegin(const char *wssUri, CxWsOnData onData, CxWsOnState onState, int rxChunk) {
  if (gTask) return true;
  if (!gSendMx) gSendMx = xSemaphoreCreateMutex();
  parseUri(wssUri);
  gOnData = onData; gOnState = onState; gRxChunk = rxChunk;
  gRunning = true; gPaused = false;
  return xTaskCreatePinnedToCore(runTask, "convex_ws", 8192, nullptr, 5, &gTask, 0) == pdPASS;
}

void cxwsEnd() {
  gRunning = false; gConnected = false;
  gTls.stop();
  for (int i = 0; i < 50 && gTask; ++i) vTaskDelay(pdMS_TO_TICKS(20));
}

void cxwsPause()  { gPaused = true; gConnected = false; gTls.stop(); }
void cxwsResume() { gPaused = false; }
