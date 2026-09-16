# Native fuzz harnesses for the Convex ESP32 client. They #include the real
# library sources with tiny shims and pound the JSON message handler and the
# WebSocket frame parser with mutated/random input under ASan+UBSan.
#
#   make run              # build + run both (500k iters each)
#   make run FUZZ_N=50000 # shorter (CI uses this)
#
# Needs a clang with sanitizers and curl (to fetch a pinned ArduinoJson header).
CXX      ?= clang++
AJ_VER   ?= 7.2.0
FUZZ_N   ?= 500000

SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer
AJFLAGS  := -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 \
            -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 -DARDUINOJSON_ENABLE_PROGMEM=0
CXXFLAGS := -std=c++17 -g -O1 -Wall -Wno-unused-function $(SAN) -DFUZZ_N=$(FUZZ_N)
INC      := -Ishim -I.aj -I../../src

.aj/ArduinoJson.h:
	mkdir -p .aj
	curl -fsSL https://github.com/bblanchon/ArduinoJson/releases/download/v$(AJ_VER)/ArduinoJson-v$(AJ_VER).h -o $@

fuzz_json: fuzz_json.cpp .aj/ArduinoJson.h $(wildcard shim/*.h shim/freertos/*.h) ../../src/convex_sync.cpp ../../src/Convex.h
	$(CXX) $(CXXFLAGS) $(AJFLAGS) $(INC) fuzz_json.cpp -o fuzz_json

# The frame parser needs no ArduinoJson.
fuzz_frames: fuzz_frames.cpp $(wildcard shim/*.h shim/freertos/*.h) ../../src/convex_ws.cpp ../../src/convex_ws.h
	$(CXX) $(CXXFLAGS) -Ishim -I../../src fuzz_frames.cpp -o fuzz_frames

run: fuzz_json fuzz_frames
	./fuzz_json
	./fuzz_frames

clean:
	rm -rf fuzz_json fuzz_frames *.dSYM .aj

.PHONY: run clean
