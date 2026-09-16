#pragma once
// Minimal Arduino shim so the library compiles natively for fuzzing. Only what
// convex_sync.cpp / convex_ws.cpp reference. Platform functions (millis, etc.)
// are DEFINED by each fuzz harness.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include "esp_heap_caps.h"

uint32_t millis();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);

// Arduino String on std::string, with the two methods ArduinoJson's
// ArduinoStringWriter needs (operator=(const char*) and concat).
class String {
  std::string s;
public:
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(const std::string &x) : s(x) {}
  String &operator=(const char *c) { s = c ? c : ""; return *this; }
  unsigned concat(const char *c) { if (c) s += c; return 1; }
  const char *c_str() const { return s.c_str(); }
  unsigned length() const { return (unsigned)s.size(); }
};
