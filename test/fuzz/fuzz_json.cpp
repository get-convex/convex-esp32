// Fuzz the Convex server-message handler (the code that parses untrusted JSON
// off the wire). Includes the unit under test directly so the static
// handleServerMessage is reachable, stubs the transport/platform, and hammers
// it with mutated and random bytes under ASan+UBSan. Fuzz inputs are raw byte
// vectors (no std::string quirks), so any crash/UB is unambiguously the library.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "convex_ws.h"

static uint32_t g_ms = 0;
uint32_t millis() { return g_ms++; }
void delay(uint32_t) {}
void delayMicroseconds(uint32_t) {}

struct FSem { };
SemaphoreHandle_t xSemaphoreCreateBinary() { return new FSem(); }
SemaphoreHandle_t xSemaphoreCreateMutex()  { return new FSem(); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return 1; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return 1; }

bool cxwsBegin(const char *, CxWsOnData, CxWsOnState, int) { return true; }
void cxwsEnd() {}
bool cxwsConnected() { return true; }
int  cxwsSendText(const char *, size_t n) { return (int)n; }
void cxwsPause() {}
void cxwsResume() {}
void cxwsPenalize() {}
void cxwsOnTick(void (*)()) {}
void cxwsSetCACertBundle(const uint8_t *, size_t) {}
void convexSetHttpBase(const char *) {}

#include "convex_sync.cpp"   // unit under test; statics become visible here

static uint64_t rngState = 0x1234567;
static uint32_t rnd() { rngState = rngState * 6364136223846793005ULL + 1; return (uint32_t)(rngState >> 33); }
static int rint(int n) { return n > 0 ? (int)(rnd() % (uint32_t)n) : 0; }

typedef std::vector<char> Bytes;
static std::vector<Bytes> corpus;
static void add(const char *s) { corpus.emplace_back(s, s + strlen(s)); }

static volatile int cbHits = 0;
static void queryCb(bool ok, JsonVariantConst v) { cbHits++; if (ok) { char b[64]; serializeJson(v, b, sizeof(b)); } }
static void resultCb(bool ok, JsonVariantConst r) { cbHits++; (void)ok; (void)r; }

static void seed() {
  add(R"({"type":"Transition","startVersion":{"querySet":0,"identity":0,"ts":"AAAAAAAAAAA="},"endVersion":{"querySet":1,"identity":0,"ts":"5i4JR5+g1Rg="},"modifications":[{"type":"QueryUpdated","queryId":0,"value":[{"userId":"a"},{"userId":"b"}],"logLines":[],"journal":null}]})");
  add(R"({"type":"Transition","endVersion":{"ts":"abc="},"modifications":[{"type":"QueryUpdated","queryId":1,"value":{"n":42,"s":"hi","arr":[1,2,3]}}]})");
  add(R"({"type":"Transition","endVersion":{"ts":"abc="},"modifications":[{"type":"QueryFailed","queryId":1,"errorMessage":"boom","errorData":null,"logLines":[],"journal":null}]})");
  add(R"({"type":"MutationResponse","requestId":0,"success":true,"result":{"roomToken":"x","sessionToken":"y"},"ts":"AA==","logLines":[]})");
  add(R"({"type":"MutationResponse","requestId":1,"success":false,"result":"error text","logLines":[],"errorData":{"a":1}})");
  add(R"({"type":"ActionResponse","requestId":2,"success":true,"result":[1,2,3],"logLines":[]})");
  add(R"({"type":"AuthError","error":"bad token","baseVersion":0,"authUpdateAttempted":true})");
  add(R"({"type":"FatalError","error":"fatal"})");
  add(R"({"type":"Ping"})");
  add(R"({"type":"TransitionChunk","chunk":"deadbeef","partNumber":0,"totalParts":3,"transitionId":"t1"})");
  add(R"({"type":"Transition","endVersion":{"ts":"z"},"modifications":[{"type":"QueryUpdated","queryId":0,"value":{"d":{"d":{"d":{"d":[[[[[1]]]]]}}}}}]})");
  add(R"({"type":"Transition","modifications":[]})");
  add(R"({})");
  add("");
}

static const size_t CAP = 32768;
static Bytes mutate(const Bytes &in) {
  Bytes b = in;
  int ops = 1 + rint(6);
  for (int i = 0; i < ops; ++i) {
    if (b.empty()) { b.push_back('{'); b.push_back('}'); }
    switch (rint(6)) {
      case 0: b[rint((int)b.size())] = (char)rnd(); break;                                  // flip
      case 1: { int p = rint((int)b.size()), n = 1 + rint(8);                               // delete run
                if (p + n > (int)b.size()) n = (int)b.size() - p;
                b.erase(b.begin() + p, b.begin() + p + n); } break;
      case 2: if (b.size() < CAP) b.insert(b.begin() + rint((int)b.size() + 1), 1 + rint(8), (char)rnd()); break;
      case 3: b.resize(rint((int)b.size() + 1)); break;                                     // truncate
      case 4: { size_t base = b.size(); if (base) while (b.size() < CAP) b.push_back(b[b.size() % base]); } break;
      case 5: b[rint((int)b.size())] = "{}[]\"\\,:0 tnf"[rint(13)]; break;                  // structural
    }
  }
  return b;
}

int main() {
  seed();
  gConnected = true;
  JsonDocument a;
  int qCb = convexSubscribe("q:cb", a, ConvexQueryFn(queryCb));   // queryId 0
  int qCache = convexSubscribe("q:cache", a);                     // queryId 1 (cached)
  convexMutation("m:x", a, ConvexResultFn(resultCb));             // requestId 0
  convexMutation("m:y", a, ConvexResultFn(resultCb));             // requestId 1
  (void)qCb;

  // Functional: a Transition split across ordered chunks must reassemble and
  // fire the callback exactly once.
  {
    std::string full = R"({"type":"Transition","endVersion":{"ts":"q"},"modifications":[{"type":"QueryUpdated","queryId":0,"value":[{"userId":"chunked"}]}]})";
    size_t h = full.size() / 2;
    auto chunk = [](int part, int total, const std::string &piece) {
      JsonDocument d; d["type"] = "TransitionChunk"; d["transitionId"] = "tc";
      d["partNumber"] = part; d["totalParts"] = total; d["chunk"] = piece;
      std::string s; serializeJson(d, s); handleServerMessage(s.data(), s.size());
    };
    int before = cbHits;
    chunk(0, 2, full.substr(0, h));
    chunk(1, 2, full.substr(h));
    printf("TransitionChunk reassembly: cbHits %d -> %d  %s\n",
           before, cbHits, cbHits > before ? "OK" : "FAIL");
  }

  #ifndef FUZZ_N
#define FUZZ_N 500000
#endif
  const int N = FUZZ_N;
  for (int i = 0; i < N; ++i) {
    Bytes msg;
    int mode = rint(10);
    if (mode < 7)      msg = mutate(corpus[rint((int)corpus.size())]);
    else if (mode < 9) { int n = rint(2048); msg.resize(n); for (auto &c : msg) c = (char)rnd(); }
    else               msg = corpus[rint((int)corpus.size())];

    handleServerMessage(msg.data(), msg.size());

    if ((i & 7) == 0) { JsonDocument out; convexQueryValue(qCache, out); convexQueryChanged(qCache); }
    if ((i & 63) == 0) { JsonDocument ev; ev["i"] = i; convexReportEvent("Fuzz", ev); }
    if ((i % 5000) == 0) { convexMutation("m:x", a, ConvexResultFn(resultCb)); convexMutation("m:y", a, ConvexResultFn(resultCb)); }
  }
  printf("fuzz_json: %d iterations OK, cbHits=%d, heapLow=%u, lastErr=%s\n",
         N, cbHits, (unsigned)convexHeapLowWater(), convexLastError());
  return 0;
}
