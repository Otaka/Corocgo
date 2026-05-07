// corocrpc.h
// Coroutine-native RPC built on corocgo.
// Transport is fully external: the caller reads outCh to send packets over the wire
// and writes inCh to deliver received packets for dispatch.
//
// Include path note: add ../../Corocgo/Corocgo (or equivalent) to your header search paths.

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "corocgo.h"  // from Corocgo project; adjust search path as needed

namespace corocrpc {

// ── StreamFramer ──────────────────────────────────────────────────────────
// Byte-stream framing layer for unreliable transports (UART, pipes, sockets).
// Independent of RpcManager — wraps any byte payload, not just RPC packets.
//
// Frame format (12-byte header + content):
//   [Magic:2][ContentSize:2][ContentCRC:2][UserData:2][Channel:2][HeaderCRC:2][Content:N]
// Magic: 0xEF 0xFE on wire (little-endian 0xFEEF). Both CRCs: CRC16-IBM.
//
// Go-like usage:
//   Send:    FramedPacket fp = framer.createPacket(ch, data, len);
//            // ship fp.data[0..fp.size-1] over the transport
//   Receive: framer.writeCh->send(chunk);   // feed raw bytes from transport
//            auto res = framer.readCh->receive();  // get next complete frame

// Input chunk sent to writeCh (one call from the transport → one RawChunk).
struct RawChunk {
    static constexpr uint16_t MAX_SIZE = 512;
    uint8_t  data[MAX_SIZE];
    uint16_t len;
};

// Output value received from readCh (one complete, CRC-validated frame).
// Sized for RPC use: RPC_PACKET_MAX (~1031 B) + HEADER_SIZE (12 B) + headroom.
// Increase if you need to frame larger payloads.
static constexpr size_t SF_BUFFER_SIZE = 2 * 1024;  // max frame (header + content)
struct FramedPacket {
    uint8_t  data[SF_BUFFER_SIZE];
    uint16_t size;     // total bytes (header + content); 0 = invalid
    uint16_t channel;
};

class StreamFramer {
public:
    static constexpr size_t HEADER_SIZE = 12;

    // Allocates writeCh and readCh, spawns the internal parse coroutine.
    // Call before scheduler_start().
    StreamFramer();
    ~StreamFramer();

    // writeCh: send RawChunks of incoming raw bytes here.
    // readCh:  receive complete FramedPackets from here.
    corocgo::Channel<RawChunk>*     writeCh;
    corocgo::Channel<FramedPacket>* readCh;

    // Synchronously build a framed packet (send direction).
    // Returns FramedPacket with size==0 on error (content too large).
    FramedPacket createPacket(uint16_t channel, const char* buffer, unsigned int length);

private:
    static constexpr size_t  BUFFER_SIZE      = SF_BUFFER_SIZE;
    static constexpr uint8_t MAGIC_BYTE_1     = 0xEF;
    static constexpr uint8_t MAGIC_BYTE_2     = 0xFE;
    static constexpr size_t  MAX_CONTENT_SIZE = BUFFER_SIZE - HEADER_SIZE;

    enum class State { SEARCH_MAGIC_1, SEARCH_MAGIC_2, READING_HEADER, READING_CONTENT };

    uint8_t  input_buffer_[BUFFER_SIZE];
    State    state_;
    size_t   bytes_received_;
    uint16_t content_size_;

    void reset();
    static uint16_t read_u16_le(const uint8_t* p);
    static void     write_u16_le(uint8_t* p, uint16_t v);
    void _emit(uint16_t channel, uint16_t totalSize);
    void _writeBytesInternal(const uint8_t* data, unsigned int length, int depth);
    void _parseLoop();
};

// ── Sizes & wire format ───────────────────────────────────────────────────
static constexpr int RPC_ARG_BUF_SIZE = 1024;
static constexpr int RPC_HEADER_SIZE  = 7;  // methodId(2) + callId(4) + flags(1)
static constexpr int RPC_PACKET_MAX   = RPC_ARG_BUF_SIZE + RPC_HEADER_SIZE;

// ── RpcArg ────────────────────────────────────────────────────────────────
// Fixed 1 KB buffer with sequential write/read cursors.
// All integers are little-endian.
struct RpcArg {
    char    buf[RPC_ARG_BUF_SIZE];
    int     writeIdx;
    int     readIdx;

    void reset();

    // Writers
    void putInt32(int32_t v);
    void putBool(bool v);
    void putString(const char* s);               // uint16 length prefix + bytes
    void putBuffer(const void* data, uint16_t len); // uint16 length prefix + bytes

    // Readers
    int32_t  getInt32();
    bool     getBool();
    // Copies string into out[outSize], null-terminates. Returns length, -1 on error.
    int      getString(char* out, int outSize);
    // Copies bytes into out. Returns actual length copied, 0 on error.
    uint16_t getBuffer(void* out, uint16_t maxLen);
};

// ── RpcPacket ─────────────────────────────────────────────────────────────
// Wire format: [methodId:uint16 LE][callId:uint32 LE][flags:uint8][payload:N]
// flags bit 0 (RPC_FLAG_IS_RESPONSE): 0 = request, 1 = response
// flags bit 1 (RPC_FLAG_NO_RESPONSE): 1 = server must not send a response
static constexpr uint8_t RPC_FLAG_IS_RESPONSE = 0x01;
static constexpr uint8_t RPC_FLAG_NO_RESPONSE = 0x02;

struct RpcPacket {
    uint8_t  data[RPC_PACKET_MAX];
    uint16_t size;
};

// ── RpcResult ─────────────────────────────────────────────────────────────
enum RpcError {
    RPC_OK      = 0,
    RPC_TIMEOUT = 1,
    RPC_CLOSED  = 2,
};

struct RpcResult {
    int     error;  // RpcError
    RpcArg* arg;    // non-null when error == RPC_OK; caller must disposeRpcArg()
};

// ── RpcManager ────────────────────────────────────────────────────────────
class RpcManager {
public:
    // outCh: RpcManager writes outbound packets here; external code reads and ships them.
    // inCh:  external code writes received packets here; RpcManager dispatches them.
    // timeoutMs: how long call() waits before returning RPC_TIMEOUT.
    //
    // Spawns internal dispatch and timeout coroutines; call before scheduler_start().
    RpcManager(corocgo::Channel<RpcPacket>* outCh,
               corocgo::Channel<RpcPacket>* inCh,
               int timeoutMs = 5000);

    ~RpcManager();

    // Server side: register a handler for methodId.
    // handler(inArg) -> outArg (or nullptr for no reply).
    // Obtain outArg via getRpcArg(); RpcManager will disposeRpcArg it after sending.
    void registerMethod(uint16_t methodId,
                        std::function<RpcArg*(RpcArg*)> handler);

    // Client side: blocking call (must run from a coroutine).
    // arg: caller-owned input; not disposed by call().
    // On RPC_OK: caller must disposeRpcArg(result.arg) when done.
    RpcResult call(uint16_t methodId, RpcArg* arg);

    // Client side: fire-and-forget — sends the request and returns immediately.
    // No response is expected; the server will not send one.
    // arg: caller-owned input; not disposed by callNoResponse().
    void callNoResponse(uint16_t methodId, RpcArg* arg);

    // Pool: obtain a zeroed RpcArg; return it when done.
    RpcArg* getRpcArg();
    void    disposeRpcArg(RpcArg* arg);

private:
    static constexpr int POOL_SIZE = 16;
    RpcArg   _pool[POOL_SIZE];
    bool     _poolUsed[POOL_SIZE];

    corocgo::Channel<RpcPacket>* _outCh;
    corocgo::Channel<RpcPacket>* _inCh;
    int      _timeoutMs;
    bool     _running;
    uint32_t _nextCallId;

    struct PendingCall {
        void*   monitor;
        RpcArg* result;
        bool    done;
        bool    timedOut;
        int64_t deadlineMs;
    };

    std::unordered_map<uint16_t, std::function<RpcArg*(RpcArg*)>> _methods;
    std::unordered_map<uint32_t, PendingCall*>                     _pending;

    static RpcPacket _makePacket(uint16_t methodId, uint32_t callId,
                                  uint8_t flags, RpcArg* arg);
    static int64_t   _nowMs();
    void _dispatchLoop();
    void _timeoutLoop();
};

// ── ChunkedRpc ────────────────────────────────────────────────────────────
// Wrapper around RpcManager that supports arbitrarily large request/response
// payloads via sequenced, idempotent RPC round-trips.

class ChunkedRpcArg {
public:
    ChunkedRpcArg() = default;
    void reset();

    void putInt32(int32_t v);
    void putBool(bool v);
    void putString(const char* s);
    void putBuffer(const void* data, uint32_t len);

    int32_t  getInt32();
    bool     getBool();
    int      getString(char* out, int outSize);
    uint32_t getBuffer(void* out, uint32_t maxLen);

    const uint8_t* data() const { return _buf.data(); }
    uint8_t*       data()       { return _buf.data(); }
    uint32_t       size() const { return static_cast<uint32_t>(_buf.size()); }
    void           resize(uint32_t n) { _buf.resize(n); }
    void           assign(const uint8_t* p, uint32_t n) { _buf.assign(p, p + n); }

private:
    std::vector<uint8_t> _buf;
    uint32_t             _readIdx = 0;
};

namespace chunked_wire {
    static constexpr uint8_t TYPE_START         = 1;
    static constexpr uint8_t TYPE_CONTINUE_REQ  = 2;
    static constexpr uint8_t TYPE_FINISH        = 3;
    static constexpr uint8_t TYPE_NEXT_RESPONSE = 5;

    static constexpr uint8_t RESP_WAITING       = 10;
    static constexpr uint8_t RESP_RESPONSE      = 11;
    static constexpr uint8_t RESP_ERROR         = 12;
    static constexpr uint8_t RESP_FINISH_ACK    = 13;

    static constexpr uint8_t ERR_UNKNOWN_SESSION = 1;
    static constexpr uint8_t ERR_BAD_OFFSET      = 2;
    static constexpr uint8_t ERR_INTERNAL        = 3;

    static constexpr uint16_t MAX_CHUNK_PAYLOAD = 512;

    int packChunk(RpcArg* outArg, uint8_t type, uint32_t sessionId,
                  const uint8_t* body, uint16_t bodyLen);

    bool parseChunkHeader(RpcArg* inArg,
                          uint8_t* type, uint32_t* sessionId,
                          const uint8_t** bodyPtr, uint16_t* bodyLen);
}

static constexpr int RPC_CHUNKED_ERROR = 100;

struct ChunkedRpcResult {
    int            error;
    int            chunkedErrorCode;
    std::string    errorMessage;
    ChunkedRpcArg* arg;
};

class ChunkedRpcManagerWrapper {
public:
    ChunkedRpcManagerWrapper(RpcManager* rpc,
                             int sessionTimeoutMs = -1,
                             int maxChunkRetries  = 3);
    ~ChunkedRpcManagerWrapper();

    void registerChunkedMethod(uint16_t methodId,
        std::function<ChunkedRpcArg*(ChunkedRpcArg*)> handler);

    ChunkedRpcResult callChunked(uint16_t methodId, ChunkedRpcArg* arg);

    ChunkedRpcArg* getChunkedArg();
    void           disposeChunkedArg(ChunkedRpcArg* arg);

    // Stops the internal GC coroutine so the scheduler can drain.
    void stop() { _gcRunning = false; }

    size_t _debug_serverSessionCount() const { return _serverSessions.size(); }

private:
    struct ServerSession {
        uint32_t             totalSize;
        uint32_t             highWaterMark;
        std::vector<uint8_t> requestBuf;
        bool                 handlerRan;
        std::vector<uint8_t> responseBuf;
        int64_t              lastActivityMs;
    };

    RpcManager* _rpc;
    int         _sessionTimeoutMs;
    int         _maxChunkRetries;
    bool        _gcRunning = false;

    std::unordered_map<uint16_t, std::function<ChunkedRpcArg*(ChunkedRpcArg*)>> _userHandlers;
    std::unordered_map<uint32_t, ServerSession> _serverSessions;

    RpcArg* _dispatchServer(uint16_t methodId, RpcArg* in);
    void    _gcLoop();

    static int64_t _nowMs();
    uint32_t _generateSessionId();
};

} // namespace corocrpc
