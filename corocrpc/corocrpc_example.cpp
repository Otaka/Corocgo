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
    METHOD_ADD            = 1,
    METHOD_NOTIFY         = 2,   // fire-and-forget; server returns no payload
    METHOD_ECHO_BUFFER    = 3,
    METHOD_PROCESS_MIXED  = 4,   // int32 + buffer -> int32 (sum of prefix + bytes + suffix)
    METHOD_REVERSE_STRING = 5,
    METHOD_SLOW           = 6,   // used for timeout test (no loopback -> never responds)

    // Two-way test: each manager registers its own method
    METHOD_LOG_A          = 10,
    METHOD_LOG_B          = 11,
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

            rpcOut->close();
            rpcIn->close();
            // give bridge coroutines a chance to exit
            coro_yield();
            delete rpcOut;
            delete rpcIn;
        }

        framer.writeCh->close();
    });

    scheduler_start();
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

    // ── Summary ───────────────────────────────────────────────────────────
    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";
    return g_failed == 0 ? 0 : 1;
}
