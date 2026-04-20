# CorocRpc

A coroutine-native RPC library for C++17 built on [corocgo].

## Files

| File | Purpose |
|------|---------|
| `corocrpc.h` | Public API: `RpcArg`, `RpcPacket`, `RpcResult`, `RpcManager`, `StreamFramer`, `Packet` |
| `corocrpc.cpp` | Implementation |
| `corocrpc_example.cpp` | Tests / usage examples |

---

## Quick Start

```cpp
#include "corocrpc.h"
using namespace corocrpc;
using namespace corocgo;

enum Method : uint16_t { METHOD_ADD = 1 };

int main() {
    auto* outCh = makeChannel<RpcPacket>(8);
    auto* inCh  = makeChannel<RpcPacket>(8);
    RpcManager rpc(outCh, inCh);

    // Server: register a method
    rpc.registerMethod(METHOD_ADD, [&rpc](RpcArg* arg) -> RpcArg* {
        int32_t a = arg->getInt32();
        int32_t b = arg->getInt32();
        RpcArg* result = rpc.getRpcArg();
        result->putInt32(a + b);
        return result;
    });

    // Loopback (in a real system: network layer reads outCh and writes inCh)
    coro([outCh, inCh]() {
        while (true) {
            auto res = outCh->receive();
            if (res.error) break;
            inCh->send(res.value);
        }
    });

    // Client: call from a coroutine
    coro([&rpc, outCh, inCh]() {
        RpcArg* arg = rpc.getRpcArg();
        arg->putInt32(10);
        arg->putInt32(20);

        RpcResult res = rpc.call(METHOD_ADD, arg);
        rpc.disposeRpcArg(arg);

        if (res.error == RPC_OK) {
            int32_t sum = res.arg->getInt32();  // 30
            rpc.disposeRpcArg(res.arg);
        }
        outCh->close(); inCh->close();
    });

    scheduler_start();
    delete outCh; delete inCh;
}
```

---

## RpcArg

Fixed 1 KB buffer with sequential write/read cursors. All multi-byte values are **little-endian**.

```cpp
RpcArg* arg = rpc.getRpcArg();   // obtain from pool
arg->reset();                     // clear (done automatically by getRpcArg)

// Writers
arg->putInt32(42);
arg->putBool(true);
arg->putString("hello");          // uint16 length prefix + bytes
arg->putBuffer(ptr, len);         // uint16 length prefix + bytes

// Readers (sequential — same order as writes)
int32_t  v = arg->getInt32();
bool     b = arg->getBool();
char     s[64]; arg->getString(s, sizeof(s));   // returns length, -1 on error
uint8_t  buf[16]; arg->getBuffer(buf, sizeof(buf)); // returns bytes copied

rpc.disposeRpcArg(arg);           // return to pool
```

Pool size is 16. `getRpcArg()` returns `nullptr` if exhausted.

---

## RpcManager

```cpp
// Constructor — spawns internal dispatch + timeout coroutines.
// Call before scheduler_start().
RpcManager rpc(outCh, inCh, timeoutMs = 5000);
```

### Channel contract

| Channel | Direction | Who writes | Who reads |
|---------|-----------|-----------|----------|
| `outCh` | outbound  | RpcManager | your transport layer |
| `inCh`  | inbound   | your transport layer | RpcManager |

External code bridges the two channels over whatever transport is in use (UART, socket, pipe, etc.).
Closing both channels shuts down the RpcManager's internal coroutines cleanly.

### Registering methods (server side)

```cpp
rpc.registerMethod(METHOD_ID, [&rpc](RpcArg* inArg) -> RpcArg* {
    // read from inArg
    int32_t x = inArg->getInt32();

    // return a result; or nullptr if there is nothing to return
    RpcArg* out = rpc.getRpcArg();
    out->putInt32(x * 2);
    return out;
    // RpcManager disposes both inArg and out after sending the response
});
```

For methods called via `callNoResponse()`, the server never sends a reply regardless of what the handler returns. Returning `nullptr` is the natural choice:

```cpp
rpc.registerMethod(METHOD_NOTIFY, [](RpcArg* inArg) -> RpcArg* {
    int32_t value = inArg->getInt32();
    // ... handle notification ...
    return nullptr;  // no response will be sent
});
```

### Making calls (client side)

```cpp
// Must be called from a coroutine — blocks (yields) until response or timeout.
RpcArg* arg = rpc.getRpcArg();
arg->putString("hello");

RpcResult res = rpc.call(METHOD_ID, arg);
rpc.disposeRpcArg(arg);   // caller disposes the input arg

if (res.error == RPC_OK) {
    char buf[64];
    res.arg->getString(buf, sizeof(buf));
    rpc.disposeRpcArg(res.arg);  // caller disposes the result arg
} else if (res.error == RPC_TIMEOUT) {
    // timed out
}
```

### Fire-and-forget calls (client side)

```cpp
// Returns immediately — no coroutine blocking, no response expected.
// The server handler is still invoked, but no response packet is ever sent.
RpcArg* arg = rpc.getRpcArg();
arg->putInt32(42);

rpc.callNoResponse(METHOD_NOTIFY, arg);
rpc.disposeRpcArg(arg);   // caller disposes the input arg
```

### RpcResult errors

| Code | Meaning |
|------|---------|
| `RPC_OK` | Success; `result.arg` may be `nullptr` if the server handler returned none |
| `RPC_TIMEOUT` | No response within `timeoutMs` |
| `RPC_CLOSED` | Input channel was closed before response arrived |

---

## Packet wire format

```
[methodId : uint16 LE]
[callId   : uint32 LE]
[flags    : uint8]      bit 0 (RPC_FLAG_IS_RESPONSE): 0=request  1=response
                        bit 1 (RPC_FLAG_NO_RESPONSE): 1=no response expected (callNoResponse)
[payload  : N bytes]    serialized RpcArg content
```

No magic bytes, no CRC — integrity and framing are the transport layer's responsibility (use `StreamFramer` if needed).

---

## StreamFramer

A standalone framing layer for byte-stream transports (UART, TCP, pipes). **Independent of RpcManager** — wraps any raw bytes, not just RPC packets. Uses corocgo channels for a Go-like interface.

### Frame format

```
[Magic:2][ContentSize:2][ContentCRC:2][UserData:2][Channel:2][HeaderCRC:2][Content:N]
```

- **Magic**: `0xEF 0xFE` on wire (little-endian `0xFEEF`)
- **ContentCRC / HeaderCRC**: CRC16-IBM
- **Channel**: logical multiplexing channel (uint16)
- **Max content**: ~10 KB

### Channels

| Channel | Type | Direction |
|---------|------|-----------|
| `writeCh` | `Channel<RawChunk>` | feed raw incoming bytes here |
| `readCh` | `Channel<FramedPacket>` | read complete validated frames from here |

`StreamFramer` creates and owns both channels. Spawns one internal parse coroutine.

### Receiving (raw bytes → complete frames)

```cpp
StreamFramer framer;

// Coroutine reading framed packets:
coro([&framer]() {
    while (true) {
        auto res = framer.readCh->receive();
        if (res.error) break;
        FramedPacket& fp = res.value;
        // content starts at fp.data + StreamFramer::HEADER_SIZE
        const uint8_t* content = fp.data + StreamFramer::HEADER_SIZE;
        uint16_t contentLen    = fp.size  - StreamFramer::HEADER_SIZE;
        // fp.channel holds the logical channel number
    }
});

// Feed raw bytes from transport (from any coroutine):
RawChunk chunk;
memcpy(chunk.data, uartBuf, bytesReceived);
chunk.len = bytesReceived;
framer.writeCh->send(chunk);

// Or from an ISR (create writeCh with extSize > 0):
framer.writeCh->sendExternalNoBlock(chunk);
```

### Sending (payload → framed bytes)

```cpp
// Synchronous — no coroutine needed
FramedPacket fp = framer.createPacket(/*channel=*/0, payload, payloadLen);
if (fp.size > 0) {
    uart_send(fp.data, fp.size);  // send framed bytes over transport
}
```

### Integrating with RpcManager

```cpp
StreamFramer framer;
auto* rpcOutCh = makeChannel<RpcPacket>(8);
auto* rpcInCh  = makeChannel<RpcPacket>(8);
RpcManager rpc(rpcOutCh, rpcInCh);

// Bridge: framer.readCh → rpcInCh (inbound: framed bytes → RPC dispatch)
coro([&framer, rpcInCh]() {
    while (true) {
        auto res = framer.readCh->receive();
        if (res.error) break;
        FramedPacket& fp = res.value;
        RpcPacket pkt;
        uint16_t contentLen = fp.size - StreamFramer::HEADER_SIZE;
        if (contentLen <= sizeof(pkt.data)) {
            memcpy(pkt.data, fp.data + StreamFramer::HEADER_SIZE, contentLen);
            pkt.size = contentLen;
            rpcInCh->send(pkt);
        }
    }
});

// Bridge: rpcOutCh → transport (outbound: RPC packets → framed bytes)
coro([&framer, rpcOutCh]() {
    while (true) {
        auto res = rpcOutCh->receive();
        if (res.error) break;
        FramedPacket fp = framer.createPacket(0,
            reinterpret_cast<const char*>(res.value.data), res.value.size);
        uart_send(fp.data, fp.size);
    }
});

// UART receive handler feeds raw bytes to the framer
void onUartReceive(const char* data, int len) {
    RawChunk chunk;
    int copy = len < RawChunk::MAX_SIZE ? len : RawChunk::MAX_SIZE;
    memcpy(chunk.data, data, copy);
    chunk.len = copy;
    framer.writeCh->send(chunk);  // from a coroutine
}
```

---

## Shutdown

Close both channels from within a coroutine to trigger clean shutdown:

```cpp
coro([outCh, inCh]() {
    // ... do work ...
    outCh->close();
    inCh->close();
    // scheduler_start() returns once all coroutines finish (within ~1s)
});
scheduler_start();
delete outCh;
delete inCh;
```

The `RpcManager` internal coroutines observe `inCh->isClosed()` and exit on their next wake cycle (dispatch loop immediately, timeout loop within 1 second).

---

---

## Streaming Mode

Enable with `#define COROCRPC_STREAMING` (or `-DCOROCRPC_STREAMING` on the compiler command line / Xcode preprocessor macros).

Streaming transfers arbitrarily large payloads that exceed the 1 KB `RpcArg` limit. Data flows as 512-byte chunks with receiver-driven flow control — the receiver signals readiness before each chunk so the sender never blocks the transport. A `corocgo::sleep(0)` yield after every chunk keeps regular `call()` latency unaffected.

### Registering a streaming method (server side)

The handler runs in its own spawned coroutine, so a slow transfer never delays regular RPC dispatch.

```cpp
rpc.registerStreamedMethod(METHOD_ID, [](RpcStreamServer& s) {
    // ── Receive phase ──────────────────────────────────────────
    std::vector<uint8_t> received;
    uint8_t chunk[512];
    while (true) {
        RpcStreamState st = s.waitReadyReceive();   // sends READY, then waits for chunk
        if (st == RpcStreamState::RECEIVE_FINISH) break;
        if (st != RpcStreamState::RECEIVE_READY)  return;  // abort/timeout
        int n = s.receive(chunk, sizeof(chunk));
        received.insert(received.end(), chunk, chunk + n);
    }

    // ── Send phase ─────────────────────────────────────────────
    // process received data, then send response
    s.sendAll(responseData, responseSize);  // helper: loops chunks + finishSend
});
```

### Making a streaming call (client side)

```cpp
RpcStreamClient sh = rpc.callStreamed(METHOD_ID);  // must run from a coroutine

// Send a large buffer
sh.sendAll(requestData, requestSize);

// Receive the response
std::vector<uint8_t> response = sh.receiveAll();
```

### Manual chunk loop (for progress tracking or partial reads)

```cpp
RpcStreamClient sh = rpc.callStreamed(METHOD_ID);

// Send phase
int offset = 0;
while (offset < totalSize) {
    if (sh.waitReadySend() != RpcStreamState::SEND_READY) return;  // abort/timeout
    offset += sh.send(buf, totalSize, offset);  // sends up to 512 bytes, returns count
}
sh.finishSend();

// Receive phase
uint8_t chunk[512];
while (true) {
    RpcStreamState st = sh.waitReadyReceive();
    if (st == RpcStreamState::RECEIVE_FINISH) break;
    if (st != RpcStreamState::RECEIVE_READY)  break;  // abort/timeout
    int n = sh.receive(chunk, sizeof(chunk));
    // ... process chunk ...
}
```

### RpcStreamState values

| Value | Meaning |
|---|---|
| `SEND_READY` | Receiver sent READY — call `send()` |
| `RECEIVE_READY` | Data chunk arrived — call `receive()` |
| `RECEIVE_FINISH` | Sender called `finishSend()` — no more data |
| `ABORTED` | Either side called `cancel()`, or an ABORT packet arrived |
| `TIMEOUT` | Wait exceeded the timeout |

### Cancellation

Either side may call `cancel()` at any point. The other side's next `wait*()` call returns `ABORTED`.

### Build with streaming enabled

```bash
clang++ -std=c++17 -DCOROCRPC_STREAMING corocgo.cpp corocrpc.cpp corocrpc_example.cpp -o test_rpc
./test_rpc
```

In Xcode: **Build Settings → Preprocessor Macros → add `COROCRPC_STREAMING`**.

---

## Build

Add to your build both `corocrpc.cpp` and `corocgo.cpp`. Add the Corocgo directory to your header search paths so `#include "corocgo.h"` resolves.

```bash
clang++ -std=c++17 corocgo.cpp corocrpc.cpp corocrpc_example.cpp -o test_rpc
./test_rpc
```
