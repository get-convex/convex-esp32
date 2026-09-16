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
static uint32_t gBytesIn = 0, gBytesOut = 0;
static ConvexStateCb gStateCb = nullptr;
static void *gStateUser = nullptr;
static bool gTelemetry = false;
static uint32_t gConnectStartMs = 0;

// Reassembly buffer for fragmented / oversized frames (PSRAM when present).
static char *gRx = nullptr;
static size_t gRxCap = 0, gRxLen = 0;

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
    if (cb) cb(success, d["result"]);
    return;
  }

  if (!strcmp(type, "AuthError")) { snprintf(gErr, sizeof(gErr), "auth: %s", d["error"] | "?"); return; }
  if (!strcmp(type, "FatalError")) {
    // The server is rejecting this connection; don't reconnect-storm it.
    snprintf(gErr, sizeof(gErr), "fatal: %s", d["error"] | "?");
    cxwsPenalize();
    return;
  }
  // Ping needs no reply; chunked transitions are only emitted for very large
  // results, which a device is unlikely to subscribe to.
  if (!strcmp(type, "TransitionChunk")) snprintf(gErr, sizeof(gErr), "unhandled TransitionChunk");
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
  if (up) { gRxLen = 0; onConnected(); }
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
    snprintf(gErr, sizeof(gErr), "low heap %u < %u; not opening TLS", freeHeap, (unsigned)CONVEX_MIN_HEAP);
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
  unlock();
  gStarted = false;
  gConnected = false;
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
  if (slot < 0) { unlock(); snprintf(gErr, sizeof(gErr), "sub table full"); return -1; }
  Sub &s = gSubs[slot];
  s.active = true;
  s.queryId = gNextQueryId++;
  snprintf(s.udfPath, sizeof(s.udfPath), "%s", udfPath);
  serializeJson(args, s.args, sizeof(s.args));
  if (!s.args[0]) strcpy(s.args, "{}");
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
  if (!gConnected) { unlock(); snprintf(gErr, sizeof(gErr), "offline"); return -1; }
  int slot = -1;
  for (int i = 0; i < MAX_REQS; ++i) if (!gReqs[i].active) { slot = i; break; }
  if (slot < 0) { unlock(); snprintf(gErr, sizeof(gErr), "req table full"); return -1; }
  Req &r = gReqs[slot];
  r.active = true;
  r.requestId = gNextRequestId++;
  r.isAction = isAction;
  r.sentMs = millis();
  snprintf(r.udfPath, sizeof(r.udfPath), "%s", udfPath);
  serializeJson(args, r.args, sizeof(r.args));
  if (!r.args[0]) strcpy(r.args, "{}");
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
