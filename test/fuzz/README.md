# Fuzzing

Native harnesses that `#include` the real library sources with small shims (see
`shim/`) and pound the two parsers that handle untrusted server data, under
AddressSanitizer + UndefinedBehaviorSanitizer:

- **`fuzz_json`** — the Convex sync **message handler** (`handleServerMessage`):
  mutated and random JSON, plus a functional check that a chunked `Transition`
  reassembles. Exercises subscription/request dispatch, the value cache, and
  `TransitionChunk` reassembly.
- **`fuzz_frames`** — the **WebSocket frame parser** (`parseFrames`): adversarial
  frame headers (64-bit lengths, masks, truncated), asserting no OOB and no
  unbounded growth.

## Run

```sh
make run                 # 500k iterations each
make run FUZZ_N=50000    # shorter
```

Needs `clang` (with sanitizers) and `curl` (fetches a pinned ArduinoJson
single-header into `.aj/`). CI runs this on every push (`.github/workflows/fuzz.yml`).
A crash, memory error, or UB fails the build.
