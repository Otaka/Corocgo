// corocrpc_example.cpp
// Comprehensive tests for corocrpc, modelled after rpc_example_main.cpp.
//
// Test topology for single-manager tests:
//   rpcManager.outCh --> loopback coroutine --> rpcManager.inCh
// (the same manager acts as both client and server)
//
// For two-way tests:
//   rpcA.outCh --> rpcB.inCh   and   rpcB.outCh --> rpcA.inCh

#include <iostream>
#include <cstring>
#include <cstdio>
#include <vector>
#include "corocrpc.h"

using namespace corocgo;
using namespace corocrpc;

// ── Test helpers ──────────────────────────────────────────────────────────
static int g_passed = 0;
static int g_failed = 0;

static void check(const char* name, bool condition) {
    if (condition) {
        std::cout << "[PASS] " << name << "\n";
        g_passed++;
    } else {
        std::cout << "[FAIL] " << name << "\n";
        g_failed++;
    }
}

static void checkInt(const char* name, int32_t expected, int32_t actual) {
    if (expected == actual) {
        std::cout << "[PASS] " << name << "\n";
        g_passed++;
    } else {
        std::cout << "[FAIL] " << name
                  << " expected=" << expected << " actual=" << actual << "\n";
        g_failed++;
    }
}

// ── Method IDs ────────────────────────────────────────────────────────────
enum MethodId : uint16_t {
    METHOD_ADD                  = 1,
    METHOD_NOTIFY               = 2,   // server returns nullptr (empty response still sent)
    METHOD_ECHO_BUFFER          = 3,
    METHOD_PROCESS_MIXED        = 4,   // int32 + buffer -> int32 (sum of prefix + bytes + suffix)
    METHOD_REVERSE_STRING       = 5,
    METHOD_SLOW                 = 6,   // used for timeout test (no loopback -> never responds)
    METHOD_NOTIFY_NO_RESPONSE   = 7,   // callNoResponse: no response packet sent at all

    // Two-way test: each manager registers its own method
    METHOD_LOG_A                = 10,
    METHOD_LOG_B                = 11,
};

// ── Loopback helpers ──────────────────────────────────────────────────────

// Standard loopback: forward every packet from src to dst immediately.
static void startLoopback(Channel<RpcPacket>* src, Channel<RpcPacket>* dst) {
    coro([src, dst]() {
        while (true) {
            auto res = src->receive();
            if (res.error) break;
            dst->send(res.value);
        }
    });
}

// Slow loopback: delays forwarding by delayMs (used for timeout test).
static void startSlowLoopback(Channel<RpcPacket>* src, Channel<RpcPacket>* dst, int delayMs) {
    coro([src, dst, delayMs]() {
        while (true) {
            auto res = src->receive();
            if (res.error) break;
            sleep(delayMs);
            dst->send(res.value);
        }
    });
}

// ── Test 0: RpcArg serialization round-trips (no RPC call needed) ─────────
static void testRpcArgRoundTrip() {
    std::cout << "\n=== Test 0: RpcArg serialization round-trips ===\n";

    RpcArg a;
    a.reset();

    // int32
    a.putInt32(12345678);
    a.putInt32(-99);
    // bool
    a.putBool(true);
    a.putBool(false);
    // string
    a.putString("hello rpc");
    // buffer
    uint8_t bytes[] = {0xDE, 0xAD, 0xBE, 0xEF};
    a.putBuffer(bytes, 4);

    // read back
    checkInt("arg/int32 positive", 12345678, a.getInt32());
    checkInt("arg/int32 negative", -99,      a.getInt32());
    check("arg/bool true",  a.getBool());
    check("arg/bool false", !a.getBool());

    char strBuf[64];
    int slen = a.getString(strBuf, sizeof(strBuf));
    check("arg/string content", strcmp(strBuf, "hello rpc") == 0);
    checkInt("arg/string length", 9, slen);

    uint8_t outBytes[8];
    uint16_t blen = a.getBuffer(outBytes, sizeof(outBytes));
    checkInt("arg/buffer length", 4, blen);
    check("arg/buffer byte[0]", outBytes[0] == 0xDE);
    check("arg/buffer byte[3]", outBytes[3] == 0xEF);
}

// ── Test 1: add(a, b) → a + b ────────────────────────────────────────────
static void testAdd(RpcManager& rpc) {
    std::cout << "\n=== Test 1: add(int32, int32) -> int32 ===\n";

    RpcArg* arg = rpc.getRpcArg();
    arg->putInt32(10);
    arg->putInt32(20);

    RpcResult res = rpc.call(METHOD_ADD, arg);
    rpc.disposeRpcArg(arg);

    checkInt("add/error",  RPC_OK, res.error);
    if (res.error == RPC_OK) {
        checkInt("add/result 10+20", 30, res.arg->getInt32());
        rpc.disposeRpcArg(res.arg);
    }

    // Negative numbers
    arg = rpc.getRpcArg();
    arg->putInt32(-7);
    arg->putInt32(3);
    res = rpc.call(METHOD_ADD, arg);
    rpc.disposeRpcArg(arg);
    checkInt("add/negative error", RPC_OK, res.error);
    if (res.error == RPC_OK) {
        checkInt("add/result -7+3", -4, res.arg->getInt32());
        rpc.disposeRpcArg(res.arg);
    }
}

// ── Test 2: notify (handler returns nullptr → no payload in response) ─────
static void testNotify(RpcManager& rpc) {
    std::cout << "\n=== Test 2: notify (fire-and-forget, no return payload) ===\n";

    RpcArg* arg = rpc.getRpcArg();
    arg->putInt32(12345);

    RpcResult res = rpc.call(METHOD_NOTIFY, arg);
    rpc.disposeRpcArg(arg);

    checkInt("notify/error",  RPC_OK,  res.error);
    check("notify/no result arg", res.arg == nullptr);
}

// ── Test 2b: callNoResponse ───────────────────────────────────────────────
static int g_noResponseCount = 0;

static void testCallNoResponse(RpcManager& rpc) {
    std::cout << "\n=== Test 2b: callNoResponse (no round-trip, handler still invoked) ===\n";

    g_noResponseCount = 0;

    RpcArg* arg = rpc.getRpcArg();
    arg->putInt32(777);
    rpc.callNoResponse(METHOD_NOTIFY_NO_RESPONSE, arg);
    rpc.disposeRpcArg(arg);

    // Yield briefly so the dispatch coroutine can process the packet.
    sleep(10);

    checkInt("callNoResponse/handler called", 1, g_noResponseCount);
}

// ── Test 3: echoBuffer ────────────────────────────────────────────────────
static void testEchoBuffer(RpcManager& rpc) {
    std::cout << "\n=== Test 3: echoBuffer (buffer in -> same buffer out) ===\n";

    uint8_t src[] = {1, 2, 3, 4, 5, 6, 7, 8};
    RpcArg* arg = rpc.getRpcArg();
    arg->putBuffer(src, sizeof(src));

    RpcResult res = rpc.call(METHOD_ECHO_BUFFER, arg);
    rpc.disposeRpcArg(arg);

    checkInt("echoBuffer/error", RPC_OK, res.error);
    if (res.error == RPC_OK) {
        uint8_t out[16];
        uint16_t len = res.arg->getBuffer(out, sizeof(out));
        checkInt("echoBuffer/length", 8, len);
        bool match = true;
        for (int i = 0; i < 8; i++) match &= (out[i] == src[i]);
        check("echoBuffer/content matches", match);
        rpc.disposeRpcArg(res.arg);
    }
}

// ── Test 4: processMixed(prefix:int32, data:buffer, suffix:int32) → sum ───
static void testProcessMixed(RpcManager& rpc) {
    std::cout << "\n=== Test 4: processMixed(int32, buffer, int32) -> int32 ===\n";

    // sum = 100 + (1+2+3+4+5) + 200 = 315
    uint8_t data[] = {1, 2, 3, 4, 5};
    RpcArg* arg = rpc.getRpcArg();
    arg->putInt32(100);
    arg->putBuffer(data, sizeof(data));
    arg->putInt32(200);

    RpcResult res = rpc.call(METHOD_PROCESS_MIXED, arg);
    rpc.disposeRpcArg(arg);

    checkInt("processMixed/error", RPC_OK, res.error);
    if (res.error == RPC_OK) {
        checkInt("processMixed/sum 100+{1..5}+200", 315, res.arg->getInt32());
        rpc.disposeRpcArg(res.arg);
    }
}

// ── Test 5: reverseString ─────────────────────────────────────────────────
static void testReverseString(RpcManager& rpc) {
    std::cout << "\n=== Test 5: reverseString(string) -> string ===\n";

    RpcArg* arg = rpc.getRpcArg();
    arg->putString("Hello RPC!");

    RpcResult res = rpc.call(METHOD_REVERSE_STRING, arg);
    rpc.disposeRpcArg(arg);

    checkInt("reverseString/error", RPC_OK, res.error);
    if (res.error == RPC_OK) {
        char buf[64];
        res.arg->getString(buf, sizeof(buf));
        check("reverseString/content", strcmp(buf, "!CPR olleH") == 0);
        rpc.disposeRpcArg(res.arg);
    }
}

// ── Test 6: timeout ───────────────────────────────────────────────────────
// Uses a slow loopback (3s delay) and a manager with 500ms timeout.
static void testTimeout() {
    std::cout << "\n=== Test 6: timeout ===\n";

    auto* outCh = makeChannel<RpcPacket>(4);
    auto* inCh  = makeChannel<RpcPacket>(4);

    // 500ms timeout; loopback delays 800ms → call must time out
    RpcManager rpc(outCh, inCh, 500);

    startSlowLoopback(outCh, inCh, 800);

    coro([&rpc, outCh, inCh]() {
        RpcArg* arg = rpc.getRpcArg();
        arg->putInt32(99);

        std::cout << "[TIMEOUT TEST] Calling slow method (expect timeout)...\n";
        RpcResult res = rpc.call(METHOD_SLOW, arg);
        rpc.disposeRpcArg(arg);

        checkInt("timeout/error is RPC_TIMEOUT", RPC_TIMEOUT, res.error);
        check("timeout/no result arg", res.arg == nullptr);

        outCh->close();
        inCh->close();
    });

    scheduler_start();
    delete outCh;
    delete inCh;
}

// ── Test 7: two-way communication ─────────────────────────────────────────
// rpcA and rpcB are both a server and a client simultaneously.
// rpcA registers METHOD_LOG_A; rpcB registers METHOD_LOG_B.
// rpcA calls METHOD_LOG_B on rpcB; rpcB calls METHOD_LOG_A on rpcA.
static void testTwoWay() {
    std::cout << "\n=== Test 7: two-way (each manager is server and client) ===\n";

    // A's output goes to B's input and vice versa.
    auto* aOut = makeChannel<RpcPacket>(8);
    auto* aIn  = makeChannel<RpcPacket>(8);
    auto* bOut = makeChannel<RpcPacket>(8);
    auto* bIn  = makeChannel<RpcPacket>(8);

    RpcManager rpcA(aOut, aIn);
    RpcManager rpcB(bOut, bIn);

    // Bridge: A->B and B->A
    startLoopback(aOut, bIn);
    startLoopback(bOut, aIn);

    static int logACallCount = 0;
    static int logBCallCount = 0;

    rpcA.registerMethod(METHOD_LOG_A, [](RpcArg* arg) -> RpcArg* {
        char msg[128];
        arg->getString(msg, sizeof(msg));
        std::cout << "[RPC_A server] log_a: " << msg << "\n";
        logACallCount++;
        return nullptr;
    });

    rpcB.registerMethod(METHOD_LOG_B, [](RpcArg* arg) -> RpcArg* {
        char msg[128];
        arg->getString(msg, sizeof(msg));
        std::cout << "[RPC_B server] log_b: " << msg << "\n";
        logBCallCount++;
        return nullptr;
    });

    // rpcA client calls rpcB's METHOD_LOG_B
    coro([&rpcA, &rpcB, aOut, aIn, bOut, bIn]() {
        {
            RpcArg* arg = rpcA.getRpcArg();
            arg->putString("Hello from A!");
            RpcResult res = rpcA.call(METHOD_LOG_B, arg);
            rpcA.disposeRpcArg(arg);
            checkInt("twoWay/A->B error", RPC_OK, res.error);
        }
        checkInt("twoWay/logB called once", 1, logBCallCount);

        {
            RpcArg* arg = rpcB.getRpcArg();
            arg->putString("Hello from B!");
            RpcResult res = rpcB.call(METHOD_LOG_A, arg);
            rpcB.disposeRpcArg(arg);
            checkInt("twoWay/B->A error", RPC_OK, res.error);
        }
        checkInt("twoWay/logA called once", 1, logACallCount);

        // Alternating calls
        for (int i = 0; i < 3; i++) {
            RpcArg* arg = rpcA.getRpcArg();
            arg->putString("A->B ping");
            RpcResult res = rpcA.call(METHOD_LOG_B, arg);
            rpcA.disposeRpcArg(arg);
            if (res.arg) rpcA.disposeRpcArg(res.arg);
        }
        checkInt("twoWay/logB total 4 calls", 4, logBCallCount);

        aOut->close(); aIn->close();
        bOut->close(); bIn->close();
    });

    scheduler_start();
    delete aOut; delete aIn;
    delete bOut; delete bIn;
}

// ── Test 8: StreamFramer ──────────────────────────────────────────────────
// Topology:
//   sender coroutine
//     → createPacket → RawChunk → framer.writeCh
//     → framer internal parse coroutine
//     → framer.readCh → receiver coroutine
//
// Tests: basic round-trip, multi-chunk fragmented delivery, corrupt byte recovery,
//        channel multiplexing (two different channel IDs in one stream).
static void testStreamFramer() {
    std::cout << "\n=== Test 8: StreamFramer ===\n";

    StreamFramer framer;

    coro([&framer]() {
        // ── Sub-test 1: basic round-trip ─────────────────────────────────
        std::cout << "[SF] Sub-test 1: basic round-trip\n";
        {
            const char* payload = "hello framer";
            FramedPacket fp = framer.createPacket(/*channel=*/0, payload, 12);
            check("sf/createPacket size > header", fp.size > StreamFramer::HEADER_SIZE);

            // Feed the whole frame as one chunk
            RawChunk chunk;
            memcpy(chunk.data, fp.data, fp.size);
            chunk.len = fp.size;
            framer.writeCh->send(chunk);

            auto res = framer.readCh->receive();
            check("sf/basic/no error",   !res.error);
            checkInt("sf/basic/channel", 0, res.value.channel);

            uint16_t contentLen = res.value.size - (uint16_t)StreamFramer::HEADER_SIZE;
            checkInt("sf/basic/content length", 12, contentLen);
            char buf[32]{};
            memcpy(buf, res.value.data + StreamFramer::HEADER_SIZE, contentLen);
            check("sf/basic/content matches", strcmp(buf, "hello framer") == 0);
        }

        // ── Sub-test 2: fragmented delivery (1 byte at a time) ───────────
        std::cout << "[SF] Sub-test 2: fragmented delivery\n";
        {
            const char* payload = "fragment me";
            FramedPacket fp = framer.createPacket(0, payload, 11);

            // Send frame byte by byte
            for (uint16_t i = 0; i < fp.size; i++) {
                RawChunk chunk;
                chunk.data[0] = fp.data[i];
                chunk.len = 1;
                framer.writeCh->send(chunk);
            }

            auto res = framer.readCh->receive();
            check("sf/frag/no error", !res.error);
            uint16_t contentLen = res.value.size - (uint16_t)StreamFramer::HEADER_SIZE;
            checkInt("sf/frag/content length", 11, contentLen);
            char buf[32]{};
            memcpy(buf, res.value.data + StreamFramer::HEADER_SIZE, contentLen);
            check("sf/frag/content matches", strcmp(buf, "fragment me") == 0);
        }

        // ── Sub-test 3: corrupt bytes before valid frame (sync recovery) ──
        std::cout << "[SF] Sub-test 3: corrupt bytes + sync recovery\n";
        {
            const char* payload = "after junk";
            FramedPacket fp = framer.createPacket(0, payload, 10);

            // Prepend 8 garbage bytes that aren't a valid frame
            RawChunk chunk;
            chunk.data[0] = 0x00; chunk.data[1] = 0xFF;
            chunk.data[2] = 0x12; chunk.data[3] = 0x34;
            chunk.data[4] = 0xAB; chunk.data[5] = 0xCD;
            chunk.data[6] = 0xEF; chunk.data[7] = 0x00;  // 0xEF alone, no 0xFE following
            memcpy(&chunk.data[8], fp.data, fp.size);
            chunk.len = (uint16_t)(8 + fp.size);
            framer.writeCh->send(chunk);

            auto res = framer.readCh->receive();
            check("sf/corrupt/no error", !res.error);
            uint16_t contentLen = res.value.size - (uint16_t)StreamFramer::HEADER_SIZE;
            checkInt("sf/corrupt/content length", 10, contentLen);
            char buf[32]{};
            memcpy(buf, res.value.data + StreamFramer::HEADER_SIZE, contentLen);
            check("sf/corrupt/content matches", strcmp(buf, "after junk") == 0);
        }

        // ── Sub-test 4: channel multiplexing ─────────────────────────────
        std::cout << "[SF] Sub-test 4: channel multiplexing\n";
        {
            FramedPacket fp0 = framer.createPacket(/*channel=*/0, "ch0", 3);
            FramedPacket fp7 = framer.createPacket(/*channel=*/7, "ch7", 3);

            // Send both frames back-to-back in one chunk
            RawChunk chunk;
            memcpy(chunk.data, fp0.data, fp0.size);
            memcpy(chunk.data + fp0.size, fp7.data, fp7.size);
            chunk.len = (uint16_t)(fp0.size + fp7.size);
            framer.writeCh->send(chunk);

            auto r0 = framer.readCh->receive();
            auto r7 = framer.readCh->receive();

            check("sf/mux/pkt0 no error", !r0.error);
            check("sf/mux/pkt7 no error", !r7.error);
            checkInt("sf/mux/pkt0 channel", 0, r0.value.channel);
            checkInt("sf/mux/pkt7 channel", 7, r7.value.channel);

            char b0[8]{}, b7[8]{};
            memcpy(b0, r0.value.data + StreamFramer::HEADER_SIZE, 3);
            memcpy(b7, r7.value.data + StreamFramer::HEADER_SIZE, 3);
            check("sf/mux/pkt0 content", strcmp(b0, "ch0") == 0);
            check("sf/mux/pkt7 content", strcmp(b7, "ch7") == 0);
        }

        // ── Sub-test 5: RpcManager round-trip through StreamFramer ────────
        // topology: rpc.outCh → frame → framer.writeCh
        //                               framer.readCh → deframe → rpc.inCh
        std::cout << "[SF] Sub-test 5: RpcManager through StreamFramer\n";
        {
            auto* rpcOut = makeChannel<RpcPacket>(4);
            auto* rpcIn  = makeChannel<RpcPacket>(4);
            RpcManager rpc(rpcOut, rpcIn);

            rpc.registerMethod(METHOD_ADD, [&rpc](RpcArg* arg) -> RpcArg* {
                int32_t a = arg->getInt32();
                int32_t b = arg->getInt32();
                RpcArg* out = rpc.getRpcArg();
                out->putInt32(a + b);
                return out;
            });

            // Outbound bridge: rpcOut → frame → framer.writeCh
            coro([rpcOut, &framer]() {
                while (true) {
                    auto res = rpcOut->receive();
                    if (res.error) break;
                    FramedPacket fp = framer.createPacket(0,
                        reinterpret_cast<const char*>(res.value.data), res.value.size);
                    RawChunk chunk;
                    memcpy(chunk.data, fp.data, fp.size);
                    chunk.len = fp.size;
                    framer.writeCh->send(chunk);
                }
            });

            // Inbound bridge: framer.readCh → deframe → rpcIn
            coro([rpcIn, &framer]() {
                while (true) {
                    auto res = framer.readCh->receive();
                    if (res.error) break;
                    RpcPacket pkt;
                    uint16_t cLen = res.value.size - (uint16_t)StreamFramer::HEADER_SIZE;
                    memcpy(pkt.data, res.value.data + StreamFramer::HEADER_SIZE, cLen);
                    pkt.size = cLen;
                    rpcIn->send(pkt);
                }
            });

            RpcArg* arg = rpc.getRpcArg();
            arg->putInt32(15);
            arg->putInt32(27);
            RpcResult result = rpc.call(METHOD_ADD, arg);
            rpc.disposeRpcArg(arg);

            checkInt("sf/rpc/error", RPC_OK, result.error);
            if (result.error == RPC_OK) {
                checkInt("sf/rpc/15+27", 42, result.arg->getInt32());
                rpc.disposeRpcArg(result.arg);
            }

            rpcOut->close();        // unblocks outbound bridge
            rpcIn->close();         // unblocks rpc._dispatchLoop
            framer.readCh->close(); // unblocks inbound bridge
            coro_yield();           // let all three coroutines exit
            coro_yield();
            delete rpcOut;
            delete rpcIn;
        }

        framer.writeCh->close();
    });

    scheduler_start();
}


// ── CRC32 helper (used by stress test) ───────────────────────────────────
static uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}


// ── Chunked-RPC loopback helpers ──────────────────────────────────────────
// Chunked tests want the loopback to close `dst` when `src` is closed so that
// the dispatch loop in ChunkedRpcManagerWrapper terminates cleanly.
static void startChunkedLoopback(Channel<RpcPacket>* src, Channel<RpcPacket>* dst) {
    coro([src, dst]() {
        while (true) {
            auto res = src->receive();
            if (res.error) { dst->close(); break; }
            if (!dst->send(res.value)) break;
        }
    });
}

static void startFlakyLoopback(Channel<RpcPacket>* src, Channel<RpcPacket>* dst, int dropCount) {
    coro([src, dst, dropCount]() {
        int dropped = 0;
        while (true) {
            auto res = src->receive();
            if (res.error) { dst->close(); break; }
            if (dropped < dropCount) { dropped++; continue; }
            if (!dst->send(res.value)) break;
        }
    });
}

// ── Test 9: ChunkedRpcArg int32 round-trip ────────────────────────────────
static void test_chunked_arg_int32_roundtrip() {
    std::cout << "\n=== Test 9: ChunkedRpcArg int32 round-trip ===\n";
    ChunkedRpcArg a;
    a.putInt32(42);
    a.putInt32(-7);
    check("ChunkedRpcArg.size after 2 int32", a.size() == 8);
    check("ChunkedRpcArg.getInt32 #1", a.getInt32() == 42);
    check("ChunkedRpcArg.getInt32 #2", a.getInt32() == -7);
}

// ── Test 10: ChunkedRpcArg buffer round-trip ──────────────────────────────
static void test_chunked_arg_buffer_roundtrip() {
    std::cout << "\n=== Test 10: ChunkedRpcArg buffer round-trip ===\n";
    ChunkedRpcArg a;
    const char* msg = "hello world";
    a.putBuffer(msg, 11);
    char out[32] = {0};
    uint32_t n = a.getBuffer(out, sizeof(out));
    check("ChunkedRpcArg.buffer length", n == 11);
    check("ChunkedRpcArg.buffer content", std::memcmp(out, msg, 11) == 0);
}

// ── Test 11: pack/parseChunk header round-trip ────────────────────────────
static void test_pack_parse_roundtrip() {
    std::cout << "\n=== Test 11: chunked_wire pack/parse round-trip ===\n";
    RpcArg arg; arg.reset();
    const uint8_t body[] = {0xAA, 0xBB, 0xCC};
    int total = chunked_wire::packChunk(&arg, chunked_wire::TYPE_START, 0xDEADBEEF, body, 3);
    check("packChunk total size", total == 7 + 3);

    uint8_t type; uint32_t sid; const uint8_t* bp; uint16_t bl;
    bool ok = chunked_wire::parseChunkHeader(&arg, &type, &sid, &bp, &bl);
    check("parseChunkHeader ok", ok);
    check("parsed type",        type == chunked_wire::TYPE_START);
    check("parsed sessionId",   sid == 0xDEADBEEFu);
    check("parsed bodyLen",     bl == 3);
    check("parsed bodyPtr[0]",  bp[0] == 0xAA);
    check("parsed bodyPtr[2]",  bp[2] == 0xCC);
}

// ── Test 12: wrapper lifecycle ────────────────────────────────────────────
static void test_wrapper_lifecycle() {
    std::cout << "\n=== Test 12: ChunkedRpcManagerWrapper lifecycle ===\n";
    Channel<RpcPacket> outCh(4), inCh(4);
    RpcManager rpc(&outCh, &inCh);
    ChunkedRpcManagerWrapper w(&rpc);
    ChunkedRpcArg* a = w.getChunkedArg();
    a->putInt32(123);
    check("wrapper getChunkedArg non-null", a != nullptr);
    check("arg writes work", a->size() == 4);
    w.disposeChunkedArg(a);
    check("wrapper lifecycle ok", true);
    w.stop();
    outCh.close();
    inCh.close();
    scheduler_start();
}

// ── Test 13: single-chunk round-trip ──────────────────────────────────────
static void test_single_chunk_roundtrip() {
    std::cout << "\n=== Test 13: single-chunk chunked round-trip ===\n";
    Channel<RpcPacket> outCh(4), inCh(4);
    RpcManager rpc(&outCh, &inCh);
    startChunkedLoopback(&outCh, &inCh);
    ChunkedRpcManagerWrapper w(&rpc);

    static constexpr uint16_t METHOD_ECHO = 100;
    w.registerChunkedMethod(METHOD_ECHO, [&](ChunkedRpcArg* in) -> ChunkedRpcArg* {
        ChunkedRpcArg* out = w.getChunkedArg();
        out->resize(in->size());
        std::memcpy(out->data(), in->data(), in->size());
        return out;
    });

    coro([&]() {
        ChunkedRpcArg* a = w.getChunkedArg();
        const char* msg = "hi";
        a->resize(2);
        std::memcpy(a->data(), msg, 2);

        ChunkedRpcResult r = w.callChunked(METHOD_ECHO, a);
        check("single-chunk error code", r.error == RPC_OK);
        check("single-chunk size",       r.arg && r.arg->size() == 2);
        check("single-chunk content",    r.arg && std::memcmp(r.arg->data(), "hi", 2) == 0);

        w.disposeChunkedArg(a);
        if (r.arg) w.disposeChunkedArg(r.arg);
        w.stop();
        outCh.close();
        inCh.close();
    });

    scheduler_start();
}

// ── Test 14: multi-chunk request ──────────────────────────────────────────
static void test_multi_chunk_request() {
    std::cout << "\n=== Test 14: multi-chunk request ===\n";
    Channel<RpcPacket> outCh(4), inCh(4);
    RpcManager rpc(&outCh, &inCh);
    startChunkedLoopback(&outCh, &inCh);
    ChunkedRpcManagerWrapper w(&rpc);

    static constexpr uint16_t METHOD_SUM = 101;
    w.registerChunkedMethod(METHOD_SUM, [&](ChunkedRpcArg* in) -> ChunkedRpcArg* {
        int32_t sum = 0;
        for (uint32_t i = 0; i < in->size(); i++) sum += in->data()[i];
        ChunkedRpcArg* out = w.getChunkedArg();
        out->putInt32(sum);
        return out;
    });

    coro([&]() {
        ChunkedRpcArg* a = w.getChunkedArg();
        a->resize(1500);
        for (uint32_t i = 0; i < 1500; i++) a->data()[i] = static_cast<uint8_t>(i & 0xFF);
        int64_t expected = 0;
        for (uint32_t i = 0; i < 1500; i++) expected += (i & 0xFF);

        ChunkedRpcResult r = w.callChunked(METHOD_SUM, a);
        check("multi-chunk req error", r.error == RPC_OK);
        if (r.error == RPC_OK) {
            int32_t got = r.arg->getInt32();
            check("multi-chunk req sum", got == static_cast<int32_t>(expected));
        }
        w.disposeChunkedArg(a);
        if (r.arg) w.disposeChunkedArg(r.arg);
        w.stop();
        outCh.close();
        inCh.close();
    });

    scheduler_start();
}

// ── Test 15: multi-chunk response ─────────────────────────────────────────
static void test_multi_chunk_response() {
    std::cout << "\n=== Test 15: multi-chunk response ===\n";
    Channel<RpcPacket> outCh(4), inCh(4);
    RpcManager rpc(&outCh, &inCh);
    startChunkedLoopback(&outCh, &inCh);
    ChunkedRpcManagerWrapper w(&rpc);

    static constexpr uint16_t METHOD_BIG = 102;
    w.registerChunkedMethod(METHOD_BIG, [&](ChunkedRpcArg* in) -> ChunkedRpcArg* {
        int32_t n = in->getInt32();
        ChunkedRpcArg* out = w.getChunkedArg();
        out->resize(n);
        for (int32_t i = 0; i < n; i++) out->data()[i] = static_cast<uint8_t>(i & 0xFF);
        return out;
    });

    coro([&]() {
        ChunkedRpcArg* a = w.getChunkedArg();
        a->putInt32(2000);
        ChunkedRpcResult r = w.callChunked(METHOD_BIG, a);
        check("multi-chunk resp error", r.error == RPC_OK);
        check("multi-chunk resp size",  r.arg && r.arg->size() == 2000);
        if (r.arg && r.arg->size() == 2000) {
            bool good = true;
            for (uint32_t i = 0; i < 2000; i++) {
                if (r.arg->data()[i] != uint8_t(i & 0xFF)) { good = false; break; }
            }
            check("multi-chunk resp content", good);
        }
        w.disposeChunkedArg(a);
        if (r.arg) w.disposeChunkedArg(r.arg);
        w.stop();
        outCh.close();
        inCh.close();
    });

    scheduler_start();
}

// ── Test 16: FINISH drains server session ─────────────────────────────────
static void test_finish_drains_server_session() {
    std::cout << "\n=== Test 16: FINISH drains server session ===\n";
    Channel<RpcPacket> outCh(4), inCh(4);
    RpcManager rpc(&outCh, &inCh);
    startChunkedLoopback(&outCh, &inCh);
    ChunkedRpcManagerWrapper w(&rpc);

    static constexpr uint16_t METHOD_ECHO = 103;
    w.registerChunkedMethod(METHOD_ECHO, [&](ChunkedRpcArg* in) -> ChunkedRpcArg* {
        ChunkedRpcArg* out = w.getChunkedArg();
        out->resize(in->size());
        std::memcpy(out->data(), in->data(), in->size());
        return out;
    });

    coro([&]() {
        ChunkedRpcArg* a = w.getChunkedArg();
        a->resize(64);
        std::memset(a->data(), 0xAB, 64);
        ChunkedRpcResult r = w.callChunked(METHOD_ECHO, a);
        check("FINISH OK roundtrip",       r.error == RPC_OK);
        check("server sessions drained",   w._debug_serverSessionCount() == 0);
        w.disposeChunkedArg(a);
        if (r.arg) w.disposeChunkedArg(r.arg);
        w.stop();
        outCh.close();
        inCh.close();
    });

    scheduler_start();
}

// ── Test 17: unknown session error ────────────────────────────────────────
static void test_unknown_session_error() {
    std::cout << "\n=== Test 17: unknown session error ===\n";
    Channel<RpcPacket> outCh(4), inCh(4);
    RpcManager rpc(&outCh, &inCh);
    startChunkedLoopback(&outCh, &inCh);
    ChunkedRpcManagerWrapper w(&rpc);

    static constexpr uint16_t METHOD_NOOP = 104;
    w.registerChunkedMethod(METHOD_NOOP, [&](ChunkedRpcArg*) -> ChunkedRpcArg* {
        return w.getChunkedArg();
    });

    coro([&]() {
        RpcArg* req = rpc.getRpcArg();
        uint8_t body[8] = {0};
        chunked_wire::packChunk(req, chunked_wire::TYPE_CONTINUE_REQ,
                                /*sid*/ 0xCAFEBABEu, body, 8);
        RpcResult r = rpc.call(METHOD_NOOP, req);
        rpc.disposeRpcArg(req);
        check("call returns OK (server replied)", r.error == RPC_OK);
        if (r.error == RPC_OK) {
            uint8_t t; uint32_t s; const uint8_t* b; uint16_t l;
            chunked_wire::parseChunkHeader(r.arg, &t, &s, &b, &l);
            check("response is ERROR",          t == chunked_wire::RESP_ERROR);
            check("error code UNKNOWN_SESSION", l >= 1 && b[0] == chunked_wire::ERR_UNKNOWN_SESSION);
            rpc.disposeRpcArg(r.arg);
        }
        w.stop();
        outCh.close();
        inCh.close();
    });

    scheduler_start();
}

// ── Test 18: chunk retry recovers from drop ───────────────────────────────
static void test_chunk_retry_recovers() {
    std::cout << "\n=== Test 18: chunk retry recovers from drop ===\n";
    Channel<RpcPacket> outCh(4), inCh(4);
    RpcManager rpc(&outCh, &inCh, /*timeoutMs*/ 200);
    startFlakyLoopback(&outCh, &inCh, /*dropCount*/ 1);
    ChunkedRpcManagerWrapper w(&rpc, /*sessionTimeoutMs*/ 5000, /*maxChunkRetries*/ 3);

    static constexpr uint16_t METHOD_ECHO = 105;
    w.registerChunkedMethod(METHOD_ECHO, [&](ChunkedRpcArg* in) -> ChunkedRpcArg* {
        ChunkedRpcArg* out = w.getChunkedArg();
        out->resize(in->size());
        std::memcpy(out->data(), in->data(), in->size());
        return out;
    });

    coro([&]() {
        ChunkedRpcArg* a = w.getChunkedArg();
        a->resize(8);
        std::memset(a->data(), 0x55, 8);
        ChunkedRpcResult r = w.callChunked(METHOD_ECHO, a);
        check("retry recovers from one drop", r.error == RPC_OK);
        check("retry payload size",           r.arg && r.arg->size() == 8);
        w.disposeChunkedArg(a);
        if (r.arg) w.disposeChunkedArg(r.arg);
        w.stop();
        outCh.close();
        inCh.close();
    });

    scheduler_start();
}

// ── Test 19: GC drops stale sessions ──────────────────────────────────────
static void test_gc_drops_stale_sessions() {
    std::cout << "\n=== Test 19: GC drops stale sessions ===\n";
    Channel<RpcPacket> outCh(4), inCh(4);
    RpcManager rpc(&outCh, &inCh, /*timeoutMs*/ 500);
    startChunkedLoopback(&outCh, &inCh);
    ChunkedRpcManagerWrapper w(&rpc, /*sessionTimeoutMs*/ 500, /*maxChunkRetries*/ 0);

    static constexpr uint16_t METHOD_BIG = 106;
    w.registerChunkedMethod(METHOD_BIG, [&](ChunkedRpcArg*) { return w.getChunkedArg(); });

    coro([&]() {
        // Send a START with TOTAL_SIZE=100 but only 8 bytes of payload — orphaned session.
        std::vector<uint8_t> body(4 + 8, 0);
        body[0] = 100;
        for (int i = 0; i < 8; i++) body[4 + i] = static_cast<uint8_t>(i);
        RpcArg* req = rpc.getRpcArg();
        chunked_wire::packChunk(req, chunked_wire::TYPE_START, /*sid*/ 0xABCDEF01u,
                                body.data(), static_cast<uint16_t>(body.size()));
        RpcResult r = rpc.call(METHOD_BIG, req);
        rpc.disposeRpcArg(req);
        if (r.error == RPC_OK) rpc.disposeRpcArg(r.arg);

        check("session present after START", w._debug_serverSessionCount() == 1);

        // gcLoop ticks every ~2s. session timeout is 500ms. Wait long enough.
        sleep(2500);
        check("session dropped by GC", w._debug_serverSessionCount() == 0);
        w.stop();
        outCh.close();
        inCh.close();
    });

    scheduler_start();
}

// ── Test 20: concurrent chunked calls ─────────────────────────────────────
static void test_concurrent_calls() {
    std::cout << "\n=== Test 20: concurrent chunked calls ===\n";
    Channel<RpcPacket> outCh(16), inCh(16);
    RpcManager rpc(&outCh, &inCh);
    startChunkedLoopback(&outCh, &inCh);
    ChunkedRpcManagerWrapper w(&rpc);

    static constexpr uint16_t METHOD_ECHO = 107;
    w.registerChunkedMethod(METHOD_ECHO, [&](ChunkedRpcArg* in) -> ChunkedRpcArg* {
        ChunkedRpcArg* out = w.getChunkedArg();
        out->resize(in->size());
        std::memcpy(out->data(), in->data(), in->size());
        return out;
    });

    static constexpr int N = 8;
    static int doneCount = 0;
    static int okCount = 0;
    doneCount = 0; okCount = 0;

    for (int i = 0; i < N; i++) {
        coro([&, i]() {
            ChunkedRpcArg* a = w.getChunkedArg();
            uint32_t sz = 600 + i * 137;
            a->resize(sz);
            for (uint32_t k = 0; k < sz; k++) a->data()[k] = static_cast<uint8_t>((k + i) & 0xFF);
            ChunkedRpcResult r = w.callChunked(METHOD_ECHO, a);
            bool ok = r.error == RPC_OK && r.arg && r.arg->size() == sz;
            if (ok) {
                for (uint32_t k = 0; k < sz; k++) {
                    if (r.arg->data()[k] != uint8_t((k + i) & 0xFF)) { ok = false; break; }
                }
            }
            if (ok) okCount++;
            doneCount++;
            w.disposeChunkedArg(a);
            if (r.arg) w.disposeChunkedArg(r.arg);
            if (doneCount == N) {
                w.stop();
                outCh.close();
                inCh.close();
            }
        });
    }

    scheduler_start();
    check("all concurrent calls finished", doneCount == N);
    check("all concurrent calls succeeded", okCount == N);
}

// ── Main ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== CoroRpc Test Suite ===\n";

    // Test 0: no RPC call needed, run directly
    testRpcArgRoundTrip();

    // Tests 1-5 share one RpcManager with a self-loopback.
    {
        auto* outCh = makeChannel<RpcPacket>(16);
        auto* inCh  = makeChannel<RpcPacket>(16);
        RpcManager rpc(outCh, inCh);

        // ── Register server methods ───────────────────────────────────────

        rpc.registerMethod(METHOD_ADD, [&rpc](RpcArg* arg) -> RpcArg* {
            int32_t a = arg->getInt32();
            int32_t b = arg->getInt32();
            std::cout << "[SERVER] add(" << a << ", " << b << ") = " << (a+b) << "\n";
            RpcArg* result = rpc.getRpcArg();
            result->putInt32(a + b);
            return result;
        });

        rpc.registerMethod(METHOD_NOTIFY, [](RpcArg* arg) -> RpcArg* {
            int32_t value = arg->getInt32();
            std::cout << "[SERVER] notify(" << value << ") - no reply\n";
            return nullptr;
        });

        rpc.registerMethod(METHOD_NOTIFY_NO_RESPONSE, [](RpcArg* arg) -> RpcArg* {
            int32_t value = arg->getInt32();
            std::cout << "[SERVER] notifyNoResponse(" << value << ") - no round-trip\n";
            g_noResponseCount++;
            return nullptr;
        });

        rpc.registerMethod(METHOD_ECHO_BUFFER, [&rpc](RpcArg* arg) -> RpcArg* {
            uint8_t buf[RPC_ARG_BUF_SIZE];
            uint16_t len = arg->getBuffer(buf, sizeof(buf));
            std::cout << "[SERVER] echoBuffer(length=" << len << ")\n";
            RpcArg* result = rpc.getRpcArg();
            result->putBuffer(buf, len);
            return result;
        });

        rpc.registerMethod(METHOD_PROCESS_MIXED, [&rpc](RpcArg* arg) -> RpcArg* {
            int32_t prefix = arg->getInt32();
            uint8_t buf[512];
            uint16_t len = arg->getBuffer(buf, sizeof(buf));
            int32_t suffix = arg->getInt32();
            int32_t sum = prefix + suffix;
            for (int i = 0; i < len; i++) sum += buf[i];
            std::cout << "[SERVER] processMixed prefix=" << prefix
                      << " len=" << len << " suffix=" << suffix << " -> " << sum << "\n";
            RpcArg* result = rpc.getRpcArg();
            result->putInt32(sum);
            return result;
        });

        rpc.registerMethod(METHOD_REVERSE_STRING, [&rpc](RpcArg* arg) -> RpcArg* {
            char str[256];
            arg->getString(str, sizeof(str));
            int len = (int)strlen(str);
            for (int i = 0, j = len-1; i < j; i++, j--) {
                char tmp = str[i]; str[i] = str[j]; str[j] = tmp;
            }
            std::cout << "[SERVER] reverseString -> \"" << str << "\"\n";
            RpcArg* result = rpc.getRpcArg();
            result->putString(str);
            return result;
        });

        startLoopback(outCh, inCh);

        // Run tests 1-5 in a single client coroutine
        coro([&rpc, outCh, inCh]() {
            testAdd(rpc);
            testNotify(rpc);
            testCallNoResponse(rpc);
            testEchoBuffer(rpc);
            testProcessMixed(rpc);
            testReverseString(rpc);
            outCh->close();
            inCh->close();
        });

        scheduler_start();
        delete outCh;
        delete inCh;
    }

    // Test 6: timeout
    testTimeout();

    // Test 7: two-way
    testTwoWay();

    // Test 8: StreamFramer
    testStreamFramer();

    // Tests 9-20: Chunked RPC wrapper
    test_chunked_arg_int32_roundtrip();
    test_chunked_arg_buffer_roundtrip();
    test_pack_parse_roundtrip();
    test_wrapper_lifecycle();
    test_single_chunk_roundtrip();
    test_multi_chunk_request();
    test_multi_chunk_response();
    test_finish_drains_server_session();
    test_unknown_session_error();
    test_chunk_retry_recovers();
    test_gc_drops_stale_sessions();
    test_concurrent_calls();

    // ── Summary ───────────────────────────────────────────────────────────
    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed == 0 ? 0 : 1;
}
