// Fuzz the WebSocket frame parser (convex_ws.cpp parseFrames): it does pointer
// arithmetic on attacker-controlled frame lengths/masks. Feed it mutated and
// random frame bytes under ASan+UBSan; any crash/OOB/UB is a real bug.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

static uint32_t g_ms = 0;
uint32_t millis() { return g_ms++; }
void delay(uint32_t) {}
void delayMicroseconds(uint32_t) {}
struct FSem {};
SemaphoreHandle_t xSemaphoreCreateBinary() { return new FSem(); }
SemaphoreHandle_t xSemaphoreCreateMutex()  { return new FSem(); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return 1; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return 1; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *, BaseType_t) { return 1; }
void vTaskDelay(TickType_t) {}
void vTaskDelete(TaskHandle_t) {}

#include "convex_ws.cpp"   // unit under test; parseFrames/queueFrame become visible

static uint64_t rs = 99; static uint32_t rnd() { rs = rs * 6364136223846793005ULL + 1; return (uint32_t)(rs >> 33); }
static int rint(int n) { return n > 0 ? (int)(rnd() % (uint32_t)n) : 0; }

static volatile size_t bytesDelivered = 0;
static void onData(uint8_t, const char *, int len, int, int) { bytesDelivered += (len > 0 ? len : 0); }

// Build a well-formed server->client frame (unmasked) with a chosen length field.
static void appendFrame(std::vector<char> &v, uint8_t opcode, const std::string &payload, int lenMode) {
  v.push_back((char)(0x80 | opcode));
  size_t n = payload.size();
  if (lenMode == 0 && n < 126) v.push_back((char)n);
  else if (lenMode == 1) { v.push_back((char)126); v.push_back((char)(n >> 8)); v.push_back((char)(n & 0xff)); }
  else { v.push_back((char)127); for (int j = 7; j >= 0; --j) v.push_back((char)((n >> (j * 8)) & 0xff)); }
  v.insert(v.end(), payload.begin(), payload.end());
}

int main() {
  gOnData = onData;
  gRxChunk = 4096;

  #ifndef FUZZ_N
#define FUZZ_N 500000
#endif
  const int N = FUZZ_N;
  for (int i = 0; i < N; ++i) {
    std::vector<char> in;
    int shape = rint(6);
    if (shape == 0) {                                   // random garbage
      int n = rint(600); in.resize(n); for (auto &c : in) c = (char)rnd();
    } else if (shape == 1) {                            // valid-ish small frame
      std::string p(rint(200), (char)rnd()); appendFrame(in, 0x1 + rint(3), p, rint(3));
    } else if (shape == 2) {                            // header claims huge length, few bytes
      in.push_back((char)0x81); in.push_back((char)127);
      for (int j = 0; j < 8; ++j) in.push_back((char)rnd());   // 64-bit length = garbage-huge
      int tail = rint(32); for (int j = 0; j < tail; ++j) in.push_back((char)rnd());
    } else if (shape == 3) {                            // masked frame (server shouldn't, but test)
      std::string p(rint(64), 'x'); in.push_back((char)0x81);
      in.push_back((char)(0x80 | p.size())); for (int j = 0; j < 4; ++j) in.push_back((char)rnd());
      in.insert(in.end(), p.begin(), p.end());
    } else if (shape == 4) {                            // ping/close/pong opcodes
      std::string p(rint(16), 'p'); appendFrame(in, 0x8 + rint(3), p, 0);
    } else {                                            // truncated header
      int n = rint(3); for (int j = 0; j < n; ++j) in.push_back((char)rnd());
    }

    // parseFrames consumes complete frames from `in`; run it as the service loop would.
    uint8_t msgOp = 0x1; std::string msg;
    std::string sin(in.begin(), in.end());
    parseFrames(sin, msgOp, msg);
    if (sin.size() > 5u * 1024 * 1024) { printf("FAIL: in grew to %zu at iter %d\n", sin.size(), i); return 2; }
  }
  printf("fuzz_frames: %d iterations OK, delivered=%zu bytes\n", N, (size_t)bytesDelivered);
  return 0;
}
