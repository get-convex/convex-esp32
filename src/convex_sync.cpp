#include "Convex.h"
#include "convex_ws.h"
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/* The Convex sync protocol, byte-for-byte as the browser client speaks it. The
 * one subtlety worth restating: a StateVersion `ts` is an opaque base64-encoded
 * u64. We never decode it -- we keep the last endVersion.ts string and echo it
 * back as maxObservedTimestamp on reconnect, which is all the server wants.
 * Query-set and identity versions are ordinary integers. Args on the wire are
 * wrapped in a one-element array: the Convex function takes one object, the
 * protocol carries [object]. */

static const char *SYNC_PROTO_VERSION = "1.45.0";

#define MAX_SUBS CONVEX_MAX_SUBS
#define MAX_REQS CONVEX_MAX_REQS
#define RX_MAX   CONVEX_RX_MAX    // largest reassembled message we will hold

struct Sub {
  bool active;
  int  queryId;
  char udfPath[CONVEX_UDF_BUF];
  char args[CONVEX_ARGS_BUF];   // serialized args object; "{}" when empty
  ConvexQueryFn cb;             // empty for a cached (poll) subscription
  bool cached;                  // keep the latest value for convexQueryValue()
  char *lastVal;               // serialized latest value (heap), null until first
  size_t lastLen;
  volatile bool changed;       // set on each update, cleared by convexQueryChanged
};

struct Req {
  bool active;
  int  requestId;
  bool isAction;           // false = mutation
  uint32_t sentMs;         // for timing out a reply that never arrives
  char udfPath[CONVEX_UDF_BUF];
  char args[CONVEX_ARGS_BUF];
  ConvexResultFn cb;
};

static bool gStarted = false;
static SemaphoreHandle_t gLock = nullptr;
static volatile bool gConnected = false;
static volatile bool gPaused = false;

static Sub gSubs[MAX_SUBS];
static Req gReqs[MAX_REQS];
static int gNextQueryId = 0;
static int gNextRequestId = 0;
static uint32_t gHeapLow = 0xffffffff;

static char gSessionId[40] = "";
static int  gConnectionCount = 0;
static char gLastCloseReason[48] = "";
static int  gQuerySetVersion = 0;
static int  gIdentityVersion = 0;
static char gToken[800] = "";
static char gMaxObservedTs[24] = "";   // opaque base64, echoed on reconnect

static char gErr[96] = "";
static ConvexStatus gStatus = CONVEX_OK;
static uint32_t gBytesIn = 0, gBytesOut = 0;
static ConvexStateCb gStateCb = nullptr;
static void *gStateUser = nullptr;
static ConvexAuthCb gAuthCb = nullptr;
static void *gAuthUser = nullptr;
static ConvexTokenFn gTokenFn = nullptr;   // proactive token provider
static void *gTokenUser = nullptr;
static uint32_t gTokenRefreshAtMs = 0;     // millis() at which to re-fetch (0 = never)
static void fetchAndSetToken(bool force);  // defined below with the auth helpers

// Record a failure: sets both the machine-readable status and the human string.
static ConvexStatus fail(ConvexStatus s, const char *msg) {
  gStatus = s; if (msg) snprintf(gErr, sizeof(gErr), "%s", msg); return s;
}
static char gErrData[192] = "";   // JSON of the last ConvexError's `errorData`
const char *convexLastErrorData() { return gErrData; }
void convexSetStatus_(ConvexStatus s) { gStatus = s; }   // internal, for convex_http.cpp
ConvexStatus convexLastStatus() { return gStatus; }
const char *convexStatusStr(ConvexStatus s) {
  switch (s) {
    case CONVEX_OK:             return "ok";
    case CONVEX_ERR_OFFLINE:    return "offline";
    case CONVEX_ERR_TABLE_FULL: return "table full";
    case CONVEX_ERR_LOW_HEAP:   return "low heap";
    case CONVEX_ERR_BAD_ARGS:   return "bad args json";
    case CONVEX_ERR_NO_BASE:    return "no deployment base";
    case CONVEX_ERR_TRANSPORT:  return "transport error";
  }
  return "?";
}
void convexOnAuthError(ConvexAuthCb cb, void *user) { gAuthCb = cb; gAuthUser = user; }
static bool gTelemetry = false;
static uint32_t gConnectStartMs = 0;

// Reassembly buffer for fragmented / oversized frames (PSRAM when present).
static char *gRx = nullptr;
static size_t gRxCap = 0, gRxLen = 0;

// Reassembly buffer for a chunked Transition (server splits very large ones).
static char *gChunk = nullptr;
static size_t gChunkCap = 0, gChunkLen = 0;
static char gChunkId[40] = "";
static int gChunkNext = 0, gChunkTotal = 0;

/* ArduinoJson out of PSRAM: sync payloads can be several KB and internal heap
 * is often spoken for by TLS on these parts. Falls back to internal RAM if the
 * chip has no PSRAM (heap_caps_malloc returns null, ArduinoJson then fails the
 * document rather than crashing). */
struct PsramAlloc : ArduinoJson::Allocator {
  void *allocate(size_t n) override {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
  }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    return q ? q : realloc(p, n);
  }
};
static PsramAlloc gJson;

static void lock()   { if (gLock) xSemaphoreTake(gLock, portMAX_DELAY); }
static void unlock() { if (gLock) xSemaphoreGive(gLock); }

static void sampleHeap() {
  uint32_t h = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (h < gHeapLow) gHeapLow = h;
}

const char *convexLastError() { return gErr; }
bool convexConnected()        { return gConnected; }
uint32_t convexBytesIn()      { return gBytesIn; }
uint32_t convexBytesOut()     { return gBytesOut; }
uint32_t convexHeapLowWater() { return gHeapLow; }
int convexSubCount() {
  int n = 0; for (int i = 0; i < MAX_SUBS; ++i) if (gSubs[i].active) ++n; return n;
}
void convexOnState(ConvexStateCb cb, void *user) { gStateCb = cb; gStateUser = user; }

static void sendText(const char *json, size_t len) {
  if (!gStarted) return;
  int w = cxwsSendText(json, len);
  if (w >= 0) gBytesOut += (uint32_t)len;
  else snprintf(gErr, sizeof(gErr), "send failed %d", w);
}

/* ---- outbound frame builders (call with gLock held) ------------------- */

static void sendConnect() {
  JsonDocument d(&gJson);
  d["type"] = "Connect";
  d["sessionId"] = gSessionId;
  d["connectionCount"] = gConnectionCount;
  if (gLastCloseReason[0]) d["lastCloseReason"] = gLastCloseReason;
  else                     d["lastCloseReason"] = (const char *)nullptr;
  if (gMaxObservedTs[0])   d["maxObservedTimestamp"] = gMaxObservedTs;
  d["clientTs"] = (double)millis();
  String s; serializeJson(d, s); sendText(s.c_str(), s.length());
}

static void sendAuth(int baseVersion) {
  if (!gToken[0]) return;
  JsonDocument d(&gJson);
  d["type"] = "Authenticate";
  d["tokenType"] = "User";
  d["value"] = gToken;
  d["baseVersion"] = baseVersion;
  String s; serializeJson(d, s); sendText(s.c_str(), s.length());
}

// Put `argsJson` (a serialized object) into `arr` as a one-element wrapper.
static void wrapArgs(JsonArray arr, const char *argsJson) {
  JsonDocument a(&gJson);
  if (deserializeJson(a, argsJson)) { arr.add(JsonObject()); return; }
  arr.add(a.as<JsonVariantConst>());
}

// restart: re-declare the whole query set at version 1 in one message.
static void sendFullQuerySet() {
  JsonDocument d(&gJson);
  d["type"] = "ModifyQuerySet";
  d["baseVersion"] = 0;
  d["newVersion"] = 1;
  JsonArray mods = d["modifications"].to<JsonArray>();
  for (int i = 0; i < MAX_SUBS; ++i) {
    if (!gSubs[i].active) continue;
    JsonObject m = mods.add<JsonObject>();
    m["type"] = "Add";
    m["queryId"] = gSubs[i].queryId;
    m["udfPath"] = gSubs[i].udfPath;
    wrapArgs(m["args"].to<JsonArray>(), gSubs[i].args);
  }
  gQuerySetVersion = 1;
  String s; serializeJson(d, s); sendText(s.c_str(), s.length());
}

static void sendAddQuery(const Sub &sub) {
  JsonDocument d(&gJson);
  d["type"] = "ModifyQuerySet";
  d["baseVersion"] = gQuerySetVersion;
  d["newVersion"] = gQuerySetVersion + 1;
  JsonObject m = d["modifications"].to<JsonArray>().add<JsonObject>();
  m["type"] = "Add";
  m["queryId"] = sub.queryId;
  m["udfPath"] = sub.udfPath;
  wrapArgs(m["args"].to<JsonArray>(), sub.args);
  gQuerySetVersion++;
  String s; serializeJson(d, s); sendText(s.c_str(), s.length());
}

static void sendRemoveQuery(int queryId) {
  JsonDocument d(&gJson);
  d["type"] = "ModifyQuerySet";
  d["baseVersion"] = gQuerySetVersion;
  d["newVersion"] = gQuerySetVersion + 1;
  JsonObject m = d["modifications"].to<JsonArray>().add<JsonObject>();
  m["type"] = "Remove";
  m["queryId"] = queryId;
  gQuerySetVersion++;
  String s; serializeJson(d, s); sendText(s.c_str(), s.length());
}

static void sendRequest(const Req &r) {
  JsonDocument d(&gJson);
  d["type"] = r.isAction ? "Action" : "Mutation";
  d["requestId"] = r.requestId;
  d["udfPath"] = r.udfPath;
  wrapArgs(d["args"].to<JsonArray>(), r.args);
  String s; serializeJson(d, s); sendText(s.c_str(), s.length());
}

// Telemetry: the protocol's Event message, as the browser client uses it.
static void sendEvent(const char *eventType, JsonVariantConst event) {
  JsonDocument d(&gJson);
  d["type"] = "Event";
  d["eventType"] = eventType;
  d["event"] = event;
  String s; serializeJson(d, s); sendText(s.c_str(), s.length());
}

/* ---- inbound dispatch ------------------------------------------------- */

// Store a query's latest value for the poll API (call with gLock held).
static void cacheValue(Sub &s, JsonVariantConst value) {
  size_t n = measureJson(value);
  if (n > (size_t)CONVEX_RX_MAX) { snprintf(gErr, sizeof(gErr), "cached value too big"); return; }
  char *nb = (char *)heap_caps_realloc(s.lastVal, n + 1, MALLOC_CAP_SPIRAM);
  if (!nb) nb = (char *)realloc(s.lastVal, n + 1);
  if (!nb) { snprintf(gErr, sizeof(gErr), "cache oom"); return; }
  s.lastVal = nb;
  serializeJson(value, s.lastVal, n + 1);
  s.lastLen = n;
  s.changed = true;
}

static void handleServerMessage(const char *json, size_t len) {
  sampleHeap();
  JsonDocument d(&gJson);
  if (deserializeJson(d, json, len)) { snprintf(gErr, sizeof(gErr), "bad server json"); return; }
  const char *type = d["type"] | "";

  if (!strcmp(type, "Transition")) {
    const char *ts = d["endVersion"]["ts"] | "";
    if (ts[0]) { lock(); snprintf(gMaxObservedTs, sizeof(gMaxObservedTs), "%s", ts); unlock(); }
    for (JsonVariantConst mod : d["modifications"].as<JsonArrayConst>()) {
      const char *mt = mod["type"] | "";
      int qid = mod["queryId"] | -1;
      bool updated = !strcmp(mt, "QueryUpdated");
      bool failed  = !strcmp(mt, "QueryFailed");
      if (!updated && !failed) continue;
      ConvexQueryFn cb;
      lock();
      for (int i = 0; i < MAX_SUBS; ++i)
        if (gSubs[i].active && gSubs[i].queryId == qid) {
          if (gSubs[i].cached && updated) cacheValue(gSubs[i], mod["value"]);
          else if (gSubs[i].cached && failed) gSubs[i].changed = true;  // poller re-checks
          cb = gSubs[i].cb;   // copy; may be empty (cached-only subscription)
          break;
        }
      unlock();
      if (!updated) { if (mod["errorData"].is<JsonVariantConst>()) serializeJson(mod["errorData"], gErrData, sizeof(gErrData)); else gErrData[0] = 0; }
      if (cb) cb(updated, updated ? mod["value"] : JsonVariantConst());
    }
    return;
  }

  if (!strcmp(type, "MutationResponse") || !strcmp(type, "ActionResponse")) {
    int rid = d["requestId"] | -1;
    bool success = d["success"] | false;
    ConvexResultFn cb;
    lock();
    for (int i = 0; i < MAX_REQS; ++i)
      if (gReqs[i].active && gReqs[i].requestId == rid) {
        cb = gReqs[i].cb; gReqs[i].active = false; break;
      }
    unlock();
    if (!success) { if (d["errorData"].is<JsonVariantConst>()) serializeJson(d["errorData"], gErrData, sizeof(gErrData)); else gErrData[0] = 0; }
    if (cb) cb(success, d["result"]);
    return;
  }

  if (!strcmp(type, "TransitionChunk")) {
    // Ordered parts, concatenated, then parsed as one Transition (as the JS
    // client does). Bounded by CONVEX_RX_MAX; a bad sequence resets and drops.
    const char *tid = d["transitionId"] | "";
    int part = d["partNumber"] | -1, total = d["totalParts"] | 0;
    const char *chunk = d["chunk"] | "";
    if (part == 0 || strcmp(tid, gChunkId) != 0) {   // start a new assembly
      gChunkLen = 0; gChunkNext = 0; gChunkTotal = total;
      snprintf(gChunkId, sizeof(gChunkId), "%s", tid);
    }
    if (part != gChunkNext || total != gChunkTotal || total <= 0) {
      gChunkLen = 0; gChunkId[0] = 0; snprintf(gErr, sizeof(gErr), "bad chunk seq"); return;
    }
    size_t clen = strlen(chunk);
    if (gChunkLen + clen > (size_t)CONVEX_RX_MAX) {
      gChunkLen = 0; gChunkId[0] = 0; snprintf(gErr, sizeof(gErr), "chunk overflow"); return;
    }
    if (gChunkLen + clen + 1 > gChunkCap) {
      size_t want = gChunkLen + clen + 512;
      char *nb = (char *)heap_caps_realloc(gChunk, want, MALLOC_CAP_SPIRAM);
      if (!nb) nb = (char *)realloc(gChunk, want);
      if (!nb) { gChunkLen = 0; gChunkId[0] = 0; snprintf(gErr, sizeof(gErr), "chunk oom"); return; }
      gChunk = nb; gChunkCap = want;
    }
    memcpy(gChunk + gChunkLen, chunk, clen); gChunkLen += clen; gChunkNext++;
    if (gChunkNext >= gChunkTotal) {                 // complete: parse as a Transition
      size_t len2 = gChunkLen; gChunkLen = 0; gChunkId[0] = 0;
      handleServerMessage(gChunk, len2);
    }
    return;
  }

  if (!strcmp(type, "AuthError")) {
    const char *err = d["error"] | "?";
    snprintf(gErr, sizeof(gErr), "auth: %s", err);
    if (gAuthCb) gAuthCb(err, gAuthUser);   // manual mode: app re-mints and convexSetAuth()
    if (gTokenFn) fetchAndSetToken(true);   // provider mode: force a fresh token now
    return;
  }
  if (!strcmp(type, "FatalError")) {
    // The server is rejecting this connection; don't reconnect-storm it.
    snprintf(gErr, sizeof(gErr), "fatal: %s", d["error"] | "?");
    cxwsPenalize();
    return;
  }
  // Ping needs no reply (handled at the frame layer); anything else is ignored.
}

/* ---- websocket event task ------------------------------------------- */

static void onConnected() {
  lock();
  gConnectionCount++;
  sendConnect();
  gIdentityVersion = 0;
  if (gToken[0]) { sendAuth(0); gIdentityVersion = 1; }
  sendFullQuerySet();                    // re-declares every live subscription
  for (int i = 0; i < MAX_REQS; ++i)     // replay in-flight mutations/actions
    if (gReqs[i].active) sendRequest(gReqs[i]);
  if (gTelemetry) {
    JsonDocument ev(&gJson);
    ev["connectionCount"] = gConnectionCount;
    ev["freeHeap"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ev["largestBlock"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (gConnectStartMs) ev["connectMs"] = (uint32_t)(millis() - gConnectStartMs);
    sendEvent("ClientConnect", ev.as<JsonVariantConst>());
  }
  unlock();
  gConnected = true;
  if (gStateCb) gStateCb(true, gStateUser);
}

static void appendRx(const char *p, size_t n) {
  if (gRxLen + n > RX_MAX) { gRxLen = 0; snprintf(gErr, sizeof(gErr), "rx overflow"); return; }
  if (gRxLen + n > gRxCap) {
    size_t want = gRxLen + n + 1024;
    char *nb = (char *)heap_caps_realloc(gRx, want, MALLOC_CAP_SPIRAM);
    if (!nb) nb = (char *)realloc(gRx, want);
    if (!nb) { gRxLen = 0; snprintf(gErr, sizeof(gErr), "rx oom"); return; }
    gRx = nb; gRxCap = want;
  }
  memcpy(gRx + gRxLen, p, n); gRxLen += n;
}

static void onWsState(bool up) {
  if (up) {
    fetchAndSetToken(false);   // refresh the token for this connection (lock not held here)
    gRxLen = 0; onConnected();
  }
  else {
    gConnected = false;
    snprintf(gLastCloseReason, sizeof(gLastCloseReason), "disconnected");
    if (gStateCb) gStateCb(false, gStateUser);
  }
}

static void onWsData(uint8_t opcode, const char *data, int len, int offset, int total) {
  // opcode 0x1 text, 0x2 binary, 0x0 continuation (ping/pong/close never reach here).
  if (len <= 0 || !data) return;
  gBytesIn += (uint32_t)len;
  // A message may span several callbacks; offset/total frame it.
  if (offset == 0) gRxLen = 0;
  appendRx(data, len);
  if (offset + len >= total && gRxLen > 0) {
    handleServerMessage(gRx, gRxLen);
    gRxLen = 0;
  }
}

/* ---- public API ------------------------------------------------------ */

static void makeSessionId() {
  uint32_t a = esp_random(), b = esp_random(), c = esp_random(), e = esp_random();
  snprintf(gSessionId, sizeof(gSessionId),
           "%08lx-%04lx-4%03lx-%04lx-%08lx%04lx",
           (unsigned long)a, (unsigned long)(b & 0xffff),
           (unsigned long)((b >> 16) & 0x0fff),
           (unsigned long)((c & 0x3fff) | 0x8000),
           (unsigned long)e, (unsigned long)(c >> 16));
}

bool convexBegin(const char *cloudUrl) {
  if (gStarted) return true;
  if (!gLock) gLock = xSemaphoreCreateMutex();
  sampleHeap();
  uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (freeHeap < (uint32_t)CONVEX_MIN_HEAP) {
    char m[64]; snprintf(m, sizeof(m), "low heap %u < %u; not opening TLS", freeHeap, (unsigned)CONVEX_MIN_HEAP);
    fail(CONVEX_ERR_LOW_HEAP, m);
    return false;
  }
  gConnectStartMs = millis();
  makeSessionId();

  // https://x.convex.cloud -> wss://x.convex.cloud/api/<ver>/sync
  const char *host = strstr(cloudUrl, "://");
  host = host ? host + 3 : cloudUrl;
  static char uri[160];
  snprintf(uri, sizeof(uri), "wss://%s/api/%s/sync", host, SYNC_PROTO_VERSION);

  // Derive the HTTP-action base (.convex.cloud -> .convex.site) for convenience.
  char site[160];
  snprintf(site, sizeof(site), "https://%s", host);
  char *dotCloud = strstr(site, ".convex.cloud");
  if (dotCloud) { memcpy(dotCloud, ".convex.site", 12); dotCloud[12] = 0; }
  convexSetHttpBase(site);

  if (!cxwsBegin(uri, onWsData, onWsState, /*rxChunk=*/4096)) {
    snprintf(gErr, sizeof(gErr), "ws start failed");
    return false;
  }
  gStarted = true;
  return true;
}

void convexEnd() {
  if (!gStarted) return;
  cxwsEnd();
  lock();
  for (int i = 0; i < MAX_SUBS; ++i) {
    gSubs[i].active = false;
    gSubs[i].cb = ConvexQueryFn();
    if (gSubs[i].lastVal) { heap_caps_free(gSubs[i].lastVal); gSubs[i].lastVal = nullptr; gSubs[i].lastLen = 0; }
  }
  for (int i = 0; i < MAX_REQS; ++i) { gReqs[i].active = false; gReqs[i].cb = ConvexResultFn(); }
  if (gChunk) { heap_caps_free(gChunk); gChunk = nullptr; gChunkCap = 0; }
  gChunkLen = 0; gChunkId[0] = 0; gTokenRefreshAtMs = 0;
  unlock();
  gStarted = false;
  gConnected = false;
}

// ---- proactive auth refresh ----

static int b64urlDecode(const char *in, size_t n, uint8_t *out, size_t cap) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
  };
  size_t o = 0; int acc = 0, bits = 0;
  for (size_t i = 0; i < n; ++i) {
    int v = val(in[i]); if (v < 0) continue;
    acc = (acc << 6) | v; bits += 6;
    if (bits >= 8) { bits -= 8; if (o >= cap) return -1; out[o++] = (uint8_t)((acc >> bits) & 0xff); }
  }
  return (int)o;
}

// Lifetime (seconds) from a JWT's exp-iat, or 0 if not derivable.
static uint32_t jwtLifetimeSec(const char *token) {
  const char *d1 = strchr(token, '.'); if (!d1) return 0;
  const char *d2 = strchr(d1 + 1, '.'); if (!d2) return 0;
  uint8_t buf[600];
  int n = b64urlDecode(d1 + 1, (size_t)(d2 - (d1 + 1)), buf, sizeof(buf) - 1);
  if (n <= 0) return 0; buf[n] = 0;
  JsonDocument doc(&gJson);
  if (deserializeJson(doc, buf, (size_t)n)) return 0;
  long exp = doc["exp"] | 0L, iat = doc["iat"] | 0L;
  return (exp > 0 && iat > 0 && exp > iat) ? (uint32_t)(exp - iat) : 0;
}

static void scheduleRefresh(const char *token) {
  if (!gTokenFn) { gTokenRefreshAtMs = 0; return; }
  uint32_t life = jwtLifetimeSec(token);
  if (!life) life = CONVEX_AUTH_DEFAULT_LIFETIME_S;
  uint32_t leeway = CONVEX_AUTH_LEEWAY_S;
  uint32_t ahead = life > leeway ? (life - leeway) : life / 2;
  gTokenRefreshAtMs = millis() + ahead * 1000;
  if (gTokenRefreshAtMs == 0) gTokenRefreshAtMs = 1;   // 0 means "never"
}

static void fetchAndSetToken(bool force) {
  if (!gTokenFn) return;
  char buf[800];
  buf[0] = 0;
  if (!gTokenFn(force, buf, sizeof(buf), gTokenUser) || !buf[0]) return;
  // If the provider hands back the token we already have, do not re-send it --
  // otherwise an AuthError on a stale token would loop (send -> AuthError ->
  // refetch same -> send ...). Just reschedule and wait for a genuinely new one.
  if (strcmp(buf, gToken) == 0) { scheduleRefresh(buf); return; }
  convexSetAuth(buf);            // stores; sends live if connected
  scheduleRefresh(buf);
}

// Runs ~once a second on the socket task (registered as the transport tick).
static void authTick() {
  if (gTokenFn && gTokenRefreshAtMs && (int32_t)(millis() - gTokenRefreshAtMs) >= 0)
    fetchAndSetToken(true);
}

void convexSetAuthProvider(ConvexTokenFn fn, void *user) {
  gTokenFn = fn; gTokenUser = user;
  cxwsOnTick(authTick);
  if (gConnected) fetchAndSetToken(false);
}

void convexSetAuth(const char *token) {
  lock();
  snprintf(gToken, sizeof(gToken), "%s", token ? token : "");
  bool live = gConnected;
  int base = gIdentityVersion;
  if (live) { sendAuth(base); gIdentityVersion++; }
  unlock();
}

static int subscribeImpl(const char *udfPath, const JsonDocument &args,
                         ConvexQueryFn cb, bool cached) {
  lock();
  int slot = -1;
  for (int i = 0; i < MAX_SUBS; ++i) if (!gSubs[i].active) { slot = i; break; }
  if (slot < 0) { unlock(); return fail(CONVEX_ERR_TABLE_FULL, "sub table full"); }
  Sub &s = gSubs[slot];
  s.active = true;
  s.queryId = gNextQueryId++;
  snprintf(s.udfPath, sizeof(s.udfPath), "%s", udfPath);
  serializeJson(args, s.args, sizeof(s.args));
  // An empty JsonDocument serializes to "null"; Convex functions take an object,
  // so an absent/null args becomes "{}" (otherwise the server rejects [null]).
  if (!s.args[0] || !strcmp(s.args, "null")) strcpy(s.args, "{}");
  s.cb = std::move(cb);
  s.cached = cached;
  s.lastVal = nullptr; s.lastLen = 0; s.changed = false;
  int qid = s.queryId;
  if (gConnected) sendAddQuery(s);
  unlock();
  return qid;
}

int convexSubscribe(const char *udfPath, const JsonDocument &args, ConvexQueryFn cb, bool cache) {
  return subscribeImpl(udfPath, args, std::move(cb), cache);
}
int convexSubscribe(const char *udfPath, const JsonDocument &args,
                    ConvexQueryCb cb, void *user, bool cache) {
  // Wrap the C-style callback; queryId is fixed once the slot is assigned.
  int qid = subscribeImpl(udfPath, args, ConvexQueryFn(), cache);
  if (qid < 0 || !cb) return qid;
  lock();
  for (int i = 0; i < MAX_SUBS; ++i)
    if (gSubs[i].active && gSubs[i].queryId == qid) {
      gSubs[i].cb = [cb, user, qid](bool ok, JsonVariantConst v) { cb(qid, ok, v, user); };
      break;
    }
  unlock();
  return qid;
}
int convexSubscribe(const char *udfPath) {
  JsonDocument empty;
  return subscribeImpl(udfPath, empty, ConvexQueryFn(), /*cache=*/true);
}
int convexSubscribe(const char *udfPath, const char *argsJson, ConvexQueryFn cb, bool cache) {
  JsonDocument a;
  if (argsJson && argsJson[0] && deserializeJson(a, argsJson))
    return fail(CONVEX_ERR_BAD_ARGS, "bad args json");
  return subscribeImpl(udfPath, a, std::move(cb), cache);
}

int convexQueryOnce(const char *udfPath, const JsonDocument &args, ConvexQueryFn cb) {
  int qid = subscribeImpl(udfPath, args, ConvexQueryFn(), /*cache=*/false);
  if (qid < 0) return qid;
  // Deliver the first result then cancel. handleServerMessage invokes a *copy*
  // of the stored callback outside the lock, so unsubscribing here (which frees
  // the stored one) does not destroy the callable mid-call.
  lock();
  for (int i = 0; i < MAX_SUBS; ++i)
    if (gSubs[i].active && gSubs[i].queryId == qid) {
      gSubs[i].cb = [cb, qid](bool ok, JsonVariantConst v) {
        if (cb) cb(ok, v);
        convexUnsubscribe(qid);
      };
      break;
    }
  unlock();
  return qid;
}
int convexQueryOnce(const char *udfPath, ConvexQueryFn cb) {
  JsonDocument empty;
  return convexQueryOnce(udfPath, empty, std::move(cb));
}

bool convexQueryChanged(int queryId) {
  bool c = false;
  lock();
  for (int i = 0; i < MAX_SUBS; ++i)
    if (gSubs[i].active && gSubs[i].queryId == queryId) { c = gSubs[i].changed; gSubs[i].changed = false; break; }
  unlock();
  return c;
}

bool convexQueryValue(int queryId, JsonDocument &out) {
  bool ok = false;
  lock();
  for (int i = 0; i < MAX_SUBS; ++i)
    if (gSubs[i].active && gSubs[i].queryId == queryId) {
      if (gSubs[i].lastVal && gSubs[i].lastLen)
        ok = (deserializeJson(out, gSubs[i].lastVal, gSubs[i].lastLen) == DeserializationError::Ok);
      break;
    }
  unlock();
  return ok;
}

void convexUnsubscribe(int queryId) {
  lock();
  for (int i = 0; i < MAX_SUBS; ++i)
    if (gSubs[i].active && gSubs[i].queryId == queryId) {
      gSubs[i].active = false;
      gSubs[i].cb = ConvexQueryFn();
      if (gSubs[i].lastVal) { heap_caps_free(gSubs[i].lastVal); gSubs[i].lastVal = nullptr; gSubs[i].lastLen = 0; }
      if (gConnected) sendRemoveQuery(queryId);
      break;
    }
  unlock();
}

// Fail and free any request whose reply never arrived, so a lost response can't
// permanently fill the table. Callbacks fire outside the lock.
static void expireRequests() {
  ConvexResultFn fire[MAX_REQS]; int nf = 0;
  lock();
  uint32_t now = millis();
  for (int i = 0; i < MAX_REQS; ++i)
    if (gReqs[i].active && (uint32_t)(now - gReqs[i].sentMs) > (uint32_t)CONVEX_REQ_TIMEOUT_MS) {
      fire[nf++] = std::move(gReqs[i].cb);
      gReqs[i].active = false; gReqs[i].cb = ConvexResultFn();
      snprintf(gErr, sizeof(gErr), "request timed out");
    }
  unlock();
  for (int i = 0; i < nf; ++i) if (fire[i]) fire[i](false, JsonVariantConst());
}

// `makeCb(rid)` builds the result callback once the requestId is assigned, so a
// raw ConvexResultCb sees the real id and the cb is in place before we send
// (no chance the response beats it).
static int enqueueRequest(const char *udfPath, const JsonDocument &args, bool isAction,
                          const std::function<ConvexResultFn(int rid)> &makeCb) {
  expireRequests();
  lock();
  if (!gConnected) { unlock(); return fail(CONVEX_ERR_OFFLINE, "offline"); }
  int slot = -1;
  for (int i = 0; i < MAX_REQS; ++i) if (!gReqs[i].active) { slot = i; break; }
  if (slot < 0) { unlock(); return fail(CONVEX_ERR_TABLE_FULL, "req table full"); }
  Req &r = gReqs[slot];
  r.active = true;
  r.requestId = gNextRequestId++;
  r.isAction = isAction;
  r.sentMs = millis();
  snprintf(r.udfPath, sizeof(r.udfPath), "%s", udfPath);
  serializeJson(args, r.args, sizeof(r.args));
  if (!r.args[0] || !strcmp(r.args, "null")) strcpy(r.args, "{}");   // [null] -> [{}]
  int rid = r.requestId;
  r.cb = makeCb(rid);
  sendRequest(r);
  unlock();
  return rid;
}

static std::function<ConvexResultFn(int)> rawResult(ConvexResultCb cb, void *user) {
  return [cb, user](int rid) -> ConvexResultFn {
    if (!cb) return ConvexResultFn();
    return [cb, user, rid](bool ok, JsonVariantConst r) { cb(rid, ok, r, user); };
  };
}
static std::function<ConvexResultFn(int)> fnResult(ConvexResultFn cb) {
  return [cb](int) -> ConvexResultFn { return cb; };
}

int convexMutation(const char *udfPath, const JsonDocument &args, ConvexResultCb cb, void *user) {
  return enqueueRequest(udfPath, args, false, rawResult(cb, user));
}
int convexAction(const char *udfPath, const JsonDocument &args, ConvexResultCb cb, void *user) {
  return enqueueRequest(udfPath, args, true, rawResult(cb, user));
}
int convexMutation(const char *udfPath, const JsonDocument &args, ConvexResultFn cb) {
  return enqueueRequest(udfPath, args, false, fnResult(std::move(cb)));
}
int convexAction(const char *udfPath, const JsonDocument &args, ConvexResultFn cb) {
  return enqueueRequest(udfPath, args, true, fnResult(std::move(cb)));
}
static int requestJson(const char *udfPath, const char *argsJson, bool isAction, ConvexResultFn cb) {
  JsonDocument a;
  if (argsJson && argsJson[0] && deserializeJson(a, argsJson))
    return fail(CONVEX_ERR_BAD_ARGS, "bad args json");
  return enqueueRequest(udfPath, a, isAction, fnResult(std::move(cb)));
}
int convexMutation(const char *udfPath, const char *argsJson, ConvexResultFn cb) {
  return requestJson(udfPath, argsJson, false, std::move(cb));
}
int convexAction(const char *udfPath, const char *argsJson, ConvexResultFn cb) {
  return requestJson(udfPath, argsJson, true, std::move(cb));
}

void convexPause() {
  if (!gStarted || gPaused) return;
  gPaused = true;
  cxwsPause();              // releases the TLS session
  gConnected = false;
}

void convexResume() {
  if (!gStarted || !gPaused) return;
  gPaused = false;
  gConnectStartMs = millis();
  cxwsResume();            // reconnect -> onConnected replays subs
}

void convexEnableTelemetry(bool on) { gTelemetry = on; }

void convexReportEvent(const char *eventType, const JsonDocument &event) {
  lock();
  if (gConnected) sendEvent(eventType, event.as<JsonVariantConst>());
  else snprintf(gErr, sizeof(gErr), "offline");
  unlock();
}
