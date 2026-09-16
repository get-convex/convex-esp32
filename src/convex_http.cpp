#include "Convex.h"
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <string.h>

/* Convex HTTP actions: the routes you register with httpAction(), served on
 * <deployment>.convex.site. esp_http_client with the IDF cert bundle for TLS.
 * An HTTP action needs its own TLS session; on tight boards it can't share the
 * heap with a live sync socket, so by default we pause the socket for the call
 * (see keepSocket). */

static char gSite[160] = "";   // https://<dep>.convex.site, set by convexBegin

void convexSetStatus_(ConvexStatus s);   // internal, defined in convex_sync.cpp

void convexSetHttpBase(const char *siteUrl) {
  snprintf(gSite, sizeof(gSite), "%s", siteUrl ? siteUrl : "");
}

static esp_http_client_method_t methodOf(const char *m) {
  if (!strcasecmp(m, "POST"))   return HTTP_METHOD_POST;
  if (!strcasecmp(m, "PUT"))    return HTTP_METHOD_PUT;
  if (!strcasecmp(m, "PATCH"))  return HTTP_METHOD_PATCH;
  if (!strcasecmp(m, "DELETE")) return HTTP_METHOD_DELETE;
  return HTTP_METHOD_GET;
}

static int httpActionRaw(const char *method, const char *pathOrUrl, const char *body,
                         const char *token, char *resp, size_t respCap,
                         const char *contentType);

int convexHttpAction(const char *method, const char *pathOrUrl, const char *body,
                     const char *token, char *resp, size_t respCap,
                     const char *contentType, bool keepSocket) {
  // Lend the single TLS session to this call unless told not to.
  bool paused = false;
  if (!keepSocket && convexConnected()) {
    convexPause();
    paused = true;
    for (int i = 0; i < 40 && convexConnected(); ++i) delay(25);  // let it release
  }
  int status = httpActionRaw(method, pathOrUrl, body, token, resp, respCap, contentType);
  if (paused) convexResume();
  return status;
}

static int httpActionRaw(const char *method, const char *pathOrUrl, const char *body,
                         const char *token, char *resp, size_t respCap,
                         const char *contentType) {
  if (resp && respCap) resp[0] = 0;

  char url[256];
  if (!strncmp(pathOrUrl, "http", 4)) {
    snprintf(url, sizeof(url), "%s", pathOrUrl);
  } else if (gSite[0]) {
    snprintf(url, sizeof(url), "%s%s%s", gSite, pathOrUrl[0] == '/' ? "" : "/", pathOrUrl);
  } else {
    convexSetStatus_(CONVEX_ERR_NO_BASE);   // caller must convexBegin() or pass a URL
    return CONVEX_ERR_NO_BASE;
  }

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 20000;
  cfg.method = methodOf(method);
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) { convexSetStatus_(CONVEX_ERR_TRANSPORT); return CONVEX_ERR_TRANSPORT; }

  if (body) esp_http_client_set_header(c, "Content-Type", contentType ? contentType : "application/json");
  if (token) {
    char auth[840];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_http_client_set_header(c, "Authorization", auth);
  }

  size_t bodyLen = body ? strlen(body) : 0;
  esp_err_t err = esp_http_client_open(c, bodyLen);
  if (err != ESP_OK) { esp_http_client_cleanup(c); convexSetStatus_(CONVEX_ERR_TRANSPORT); return CONVEX_ERR_TRANSPORT; }
  if (bodyLen) {
    int w = esp_http_client_write(c, body, bodyLen);
    if (w < 0) { esp_http_client_close(c); esp_http_client_cleanup(c); convexSetStatus_(CONVEX_ERR_TRANSPORT); return CONVEX_ERR_TRANSPORT; }
  }
  esp_http_client_fetch_headers(c);
  int status = esp_http_client_get_status_code(c);
  if (resp && respCap > 1) {
    int r = esp_http_client_read_response(c, resp, (int)respCap - 1);
    resp[r > 0 ? r : 0] = 0;
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return status;
}
