#pragma once
/* Compile-time capacity and memory knobs. Override any of these with a -D flag
 * (PlatformIO build_flags / Arduino build options) to fit a tighter or roomier
 * board. Defaults target an ESP32-S3 with PSRAM. */

// How many concurrent query subscriptions and in-flight mutations/actions.
#ifndef CONVEX_MAX_SUBS
#define CONVEX_MAX_SUBS 12
#endif
#ifndef CONVEX_MAX_REQS
#define CONVEX_MAX_REQS 8
#endif

// Per-slot buffers: the "file:function" path and the serialized args object.
#ifndef CONVEX_UDF_BUF
#define CONVEX_UDF_BUF 72
#endif
#ifndef CONVEX_ARGS_BUF
#define CONVEX_ARGS_BUF 320
#endif

// Largest single server message we will reassemble. Past this the message is
// dropped with an error rather than growing the heap without bound. Smaller by
// default without PSRAM, where the buffer comes out of scarce internal RAM.
#ifndef CONVEX_RX_MAX
#  if defined(BOARD_HAS_PSRAM)
#    define CONVEX_RX_MAX (48 * 1024)
#  else
#    define CONVEX_RX_MAX (8 * 1024)
#  endif
#endif

// Refuse to open (or reopen) the TLS session when free internal heap is below
// this, so we fail with a clear error instead of crashing mid-handshake.
#ifndef CONVEX_MIN_HEAP
#define CONVEX_MIN_HEAP 45000
#endif

// Cap the outbound send queue; past this the oldest frame is dropped so a
// stalled socket cannot grow the heap without bound.
#ifndef CONVEX_SEND_QUEUE_MAX
#define CONVEX_SEND_QUEUE_MAX 16
#endif

// A mutation/action whose response never arrives is failed and its slot freed
// after this many ms, so a lost reply cannot permanently fill the request table.
#ifndef CONVEX_REQ_TIMEOUT_MS
#define CONVEX_REQ_TIMEOUT_MS 30000
#endif
