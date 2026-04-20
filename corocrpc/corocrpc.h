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

#ifdef COROCRPC_STREAMING
static constexpr uint8_t RPC_FLAG_IS_STREAM      = 0x04;
static constexpr uint8_t RPC_FLAG_STREAM_READY   = 0x08;
static constexpr uint8_t RPC_FLAG_STREAM_END     = 0x10;
static constexpr uint8_t RPC_FLAG_STREAM_ABORT   = 0x20;
// RPC_FLAG_STREAM_TIMEOUT is synthesised internally by _timeoutLoop(); never goes over the wire.
static constexpr uint8_t RPC_FLAG_STREAM_TIMEOUT = 0x40;

static constexpr int RPC_STREAM_CHUNK_SIZE = 512;
#endif // COROCRPC_STREAMING

struct RpcPacket {
    uint8_t  data[RPC_PACKET_MAX];
    uint16_t size;
};

// ── RpcResult ─────────────────────────────────────────────────────────────
#ifdef COROCRPC_STREAMING
// Forward declarations for stream classes
class RpcStreamClient;
class RpcStreamServer;
#endif // COROCRPC_STREAMING

enum RpcError {
    RPC_OK      = 0,
    RPC_TIMEOUT = 1,
    RPC_CLOSED  = 2,
};

struct RpcResult {
    int     error;  // RpcError
    RpcArg* arg;    // non-null when error == RPC_OK; caller must disposeRpcArg()
};

#ifdef COROCRPC_STREAMING
enum class RpcStreamState {
    SEND_READY,      // READY received from receiver — can call send()
    RECEIVE_READY,   // data chunk arrived — can call receive()
    RECEIVE_FINISH,  // END received — sender has no more data
    ABORTED,         // ABORT received or cancel() called
    TIMEOUT          // wait exceeded timeout
};

// Internal session shared between RpcStreamClient/Server and RpcManager dispatch.
struct RpcStreamSession {
    uint32_t                     streamId;
    uint16_t                     methodId;
    corocgo::Channel<RpcPacket>* inCh;
    int64_t                      waitDeadlineMs; // 0 = not waiting
};

class RpcStreamClient {
public:
    RpcStreamClient(RpcStreamSession* session,
                    corocgo::Channel<RpcPacket>* outCh,
                    uint16_t methodId,
                    uint32_t streamId,
                    int timeoutMs,
                    std::function<void(uint32_t)> cleanup);

    // Request-send phase: wait for server READY, then send one chunk (<=512 bytes).
    RpcStreamState waitReadySend(int timeoutMs = -1);
    int            send(const uint8_t* buf, int size, int offset); // returns bytes sent
    void           finishSend();

    // Response-receive phase: signal server we are ready, then read arriving chunk.
    RpcStreamState waitReadyReceive(int timeoutMs = -1);
    int            receive(uint8_t* buf, int maxSize); // returns bytes read

    void cancel();

    // Helpers
    void                 sendAll(const uint8_t* buf, int size);
    std::vector<uint8_t> receiveAll();

private:
    RpcStreamSession*            _session;
    corocgo::Channel<RpcPacket>* _outCh;
    uint16_t                     _methodId;
    uint32_t                     _streamId;
    int                          _timeoutMs;
    bool                         _sendFinished;
    RpcPacket                    _pendingPacket;
    bool                         _hasPendingPacket;
    std::function<void(uint32_t)> _cleanup;
};

class RpcStreamServer {
public:
    RpcStreamServer(RpcStreamSession* session,
                    corocgo::Channel<RpcPacket>* outCh,
                    uint16_t methodId,
                    uint32_t streamId,
                    int timeoutMs);

    // Request-receive phase: send READY to client, then wait for chunk.
    RpcStreamState waitReadyReceive(int timeoutMs = -1);
    int            receive(uint8_t* buf, int maxSize); // returns bytes read

    // Response-send phase: wait for client READY, then send one chunk.
    RpcStreamState waitReadySend(int timeoutMs = -1);
    int            send(const uint8_t* buf, int size, int offset); // returns bytes sent
    void           finishSend();

    void cancel();

    // Helper
    void sendAll(const uint8_t* buf, int size);

private:
    RpcStreamSession*            _session;
    corocgo::Channel<RpcPacket>* _outCh;
    uint16_t                     _methodId;
    uint32_t                     _streamId;
    int                          _timeoutMs;
    bool                         _sendFinished;
    RpcPacket                    _pendingPacket;
    bool                         _hasPendingPacket;
};
#endif // COROCRPC_STREAMING

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

#ifdef COROCRPC_STREAMING
    // Client side: open a streaming session (must run from a coroutine).
    RpcStreamClient callStreamed(uint16_t methodId);

    // Server side: register a streaming handler for methodId.
    void registerStreamedMethod(uint16_t methodId,
                                std::function<void(RpcStreamServer&)> handler);
#endif // COROCRPC_STREAMING

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
#ifdef COROCRPC_STREAMING
    std::unordered_map<uint32_t, RpcStreamSession*>               _streamSessions;
    std::unordered_map<uint16_t, std::function<void(RpcStreamServer&)>> _streamMethods;
#endif // COROCRPC_STREAMING

    static RpcPacket _makePacket(uint16_t methodId, uint32_t callId,
                                  uint8_t flags, RpcArg* arg);
    static int64_t   _nowMs();
    void _dispatchLoop();
    void _timeoutLoop();
};

} // namespace corocrpc
