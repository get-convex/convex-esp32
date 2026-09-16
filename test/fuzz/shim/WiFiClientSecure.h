#pragma once
// No-op WiFiClientSecure so convex_ws.cpp compiles natively; the socket is
// never actually driven during frame-parser fuzzing.
#include <stdint.h>
#include <stddef.h>
class WiFiClientSecure {
public:
  void setCACertBundle(const uint8_t *, size_t) {}
  void setCACertBundle(const uint8_t *) {}
  void setInsecure() {}
  void setHandshakeTimeout(int) {}
  int  connect(const char *, uint16_t) { return 0; }
  int  connect(const char *, int) { return 0; }
  bool connected() { return false; }
  int  available() { return 0; }
  int  read(uint8_t *, size_t) { return -1; }
  int  read() { return -1; }
  size_t write(const uint8_t *, size_t n) { return n; }
  void stop() {}
};
