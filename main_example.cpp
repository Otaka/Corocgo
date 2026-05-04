//  main_example.cpp

#include "corocgo.h"
#include <stdio.h>
#include <unistd.h>
#include <string>
#include <cassert>
#include <thread>

using namespace std;
using namespace corocgo;

// ── helpers ──────────────────────────────────────────────────────────────────

static int passed = 0;
static int failed = 0;

static void check(bool cond, const char* label) {
    if (cond) {
        printf("  [PASS] %s\n", label);
        passed++;
    } else {
        printf("  [FAIL] %s\n", label);
        failed++;
    }
}

// ── test: simple coroutine ───────────────────────────────────────────────────
// Verifies that a coroutine runs, can yield, and that two coroutines
// interleave correctly via coro_yield().

static void test_simple_coroutine() {
    printf("\n[test_simple_coroutine]\n");

    auto* log = makeChannel<int>(8);  // log of events in order

    coro([log]() {
        log->send(1);
        coro_yield();
        log->send(3);
        log->send(4);
        log->close();
    });

    coro([log]() {
        log->send(2);
    });

    scheduler_start();

    // Drain log and verify order
    int expected[] = {1, 2, 3, 4};
    bool order_ok = true;
    for (int e : expected) {
        auto [v, err] = log->tryReceive();
        if (err || v != e) { order_ok = false; break; }
    }
    check(order_ok, "coroutines interleave correctly across yield");
    delete log;
}

// ── test: channel ────────────────────────────────────────────────────────────
// Verifies buffered send/receive, blocking behaviour, and close semantics.

static void test_channel() {
    printf("\n[test_channel]\n");

    // --- basic send / receive ---
    {
        auto* ch = makeChannel<int>(4);
        auto* results = makeChannel<int>(8);

        coro([ch]() {
            for (int i = 1; i <= 4; i++)
                ch->send(i);
            ch->close();
        });

        coro([ch, results]() {
            while (true) {
                auto [v, err] = ch->receive();
                if (err) break;
                results->send(v);
            }
            results->close();
        });

        scheduler_start();

        int sum = 0;
        while (true) {
            auto [v, err] = results->tryReceive();
            if (err) break;
            sum += v;
        }
        check(sum == 10, "receive all sent values (sum==10)");
        delete ch;
        delete results;
    }

    // --- blocking: sender blocks when buffer full, receiver unblocks it ---
    {
        auto* ch  = makeChannel<int>(1);   // capacity 1 — second send must block
        auto* log = makeChannel<int>(8);

        coro([ch, log]() {
            ch->send(1);     log->send(1);   // fits immediately
            ch->send(2);     log->send(2);   // blocks until receiver reads slot 1
            ch->close();
        });

        coro([ch, log]() {
            sleep(50);                       // let sender fill the buffer first
            auto [v1, _1] = ch->receive();
            auto [v2, _2] = ch->receive();
            log->send(v1 * 10 + v2);        // encode both values
            log->close();
        });

        scheduler_start();

        auto [e1, _a] = log->tryReceive();
        auto [e2, _b] = log->tryReceive();
        auto [e3, _c] = log->tryReceive();
        check(e1 == 1 && e2 == 2 && e3 == 12,
              "sender blocks on full channel, receiver unblocks it");
        delete ch;
        delete log;
    }

    // --- close propagates to receiver ---
    {
        auto* ch = makeChannel<string>(2);
        int received = 0;
        bool got_close = false;

        coro([ch]() {
            ch->send("a");
            ch->send("b");
            ch->close();
        });

        coro([ch, &received, &got_close]() {
            while (true) {
                auto [v, err] = ch->receive();
                if (err) { got_close = true; break; }
                received++;
            }
        });

        scheduler_start();

        check(received == 2,    "received all values before close");
        check(got_close,        "close detected by receiver");
        delete ch;
    }
}

// ── test: write from external thread ────────────────────────────────────────
// Uses exec_thread to run a blocking operation off the scheduler thread and
// signal completion via the wake callback.  Also tests sendExternalNoBlock.

static void test_external_thread() {
    printf("\n[test_external_thread]\n");

    // --- exec_thread wake callback ---
    {
        bool work_done = false;
        int result = 0;

        coro([&]() {
            exec_thread([&](auto wake) {
                // this runs on a pool thread
                this_thread::sleep_for(chrono::milliseconds(20));
                work_done = true;
                result = 42;
                wake();
            });
            check(work_done, "exec_thread: work completed before coroutine resumed");
            check(result == 42, "exec_thread: result value is correct");
        });

        scheduler_start();
    }

    // --- sendExternalNoBlock from a real OS thread ---
    {
        auto* ch = makeChannel<int>(8, 8);   // ext buffer enabled
        auto* results = makeChannel<int>(8);

        coro([ch, results]() {
            // kick off a thread that sends values externally
            exec_thread([ch](auto wake) {
                for (int i = 1; i <= 5; i++) {
                    this_thread::sleep_for(chrono::milliseconds(10));
                    while (!ch->sendExternalNoBlock(i)) {}  // retry if full
                }
                wake();
            });
            ch->close();
        });

        coro([ch, results]() {
            while (true) {
                auto [v, err] = ch->receive();
                if (err) break;
                results->send(v);
            }
            results->close();
        });

        scheduler_start();

        int sum = 0, count = 0;
        while (true) {
            auto [v, err] = results->tryReceive();
            if (err) break;
            sum += v; count++;
        }
        check(count == 5,   "sendExternalNoBlock: all 5 values received");
        check(sum  == 15,   "sendExternalNoBlock: sum of values is correct");
        delete ch;
        delete results;
    }
}

// ── test: wait_file with a pipe ──────────────────────────────────────────────
// Creates a POSIX pipe; one coroutine waits for the read end to become ready,
// another writes to the write end via exec_thread (off-scheduler thread so
// the write is truly asynchronous from the scheduler's perspective).

static void test_wait_file() {
    printf("\n[test_wait_file]\n");

    int pipefd[2];
    assert(pipe(pipefd) == 0);   // pipefd[0]=read, pipefd[1]=write

    auto* log = makeChannel<int>(4);

    // Writer: runs on a pool thread, writes after a short delay
    coro([&pipefd, log]() {
        exec_thread([&pipefd, log](auto wake) {
            this_thread::sleep_for(chrono::milliseconds(30));
            const char msg[] = "hello";
            write(pipefd[1], msg, sizeof(msg));
            wake();
        });
        log->send(2);   // written
    });

    // Reader: suspends via wait_file until data is available
    coro([&pipefd, log]() {
        log->send(1);   // about to wait
        auto [flags, err] = wait_file(pipefd[0], WAIT_IN);
        check(err == 0,              "wait_file: no error on pipe read-ready");
        check(flags & WAIT_IN,       "wait_file: WAIT_IN flag set when pipe has data");

        char buf[16] = {};
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        check(n > 0 && string(buf) == "hello", "wait_file: correct data read from pipe");

        log->send(3);   // done reading
        log->close();
    });

    scheduler_start();

    // Verify ordering: 1 (reader waiting) → 2 (writer wrote) → 3 (reader done)
    int e1 = log->tryReceive().value;
    int e2 = log->tryReceive().value;
    int e3 = log->tryReceive().value;
    check(e1 == 1 && e2 == 2 && e3 == 3,
          "wait_file: reader and writer interleaved in correct order");

    close(pipefd[0]);
    close(pipefd[1]);
    delete log;
}

// ── test: coro_sleep_wake / CoroSleepHandler ──────────────────────────────────
// Simulates an external system (e.g. a GUI framework callback) waking a
// coroutine.  The coroutine gets a handler, passes the wake side to a thread,
// and calls sleep() to suspend.  The thread fires wake() after a short delay.

static void test_coro_sleep_wake() {
    printf("\n[test_coro_sleep_wake]\n");

    // --- basic wake from external thread ---
    {
        bool work_done = false;
        int  result    = 0;

        coro([&]() {
            CoroSleepHandler h = coro_sleep_wake();

            // Hand the handle to an OS thread (simulates a GUI callback, Qt signal, etc.)
            thread([h, &work_done, &result]() {
                this_thread::sleep_for(chrono::milliseconds(1000));
                work_done = true;
                result    = 99;
                h.wake();   // resumes the coroutine from the OS thread
            }).detach();

            h.sleep();   // suspend — will not run again until h.wake() is called

            check(work_done, "coro_sleep_wake: work completed before coroutine resumed");
            check(result == 99, "coro_sleep_wake: result value is correct");
        });

        scheduler_start();
    }

    // --- wake called before sleep (race-safe) ---
    {
        // If the external caller fires wake() before sleep(), the scheduler
        // must still resume the coroutine exactly once.
        bool resumed = false;

        coro([&]() {
            CoroSleepHandler h = coro_sleep_wake();
            h.wake();    // fire before sleep — should be safe
            h.sleep();
            resumed = true;
        });

        scheduler_start();
        check(resumed, "coro_sleep_wake: wake-before-sleep still resumes coroutine");
    }
}

// ── test: select on external channels ────────────────────────────────────────

static void test_select_external() {
    printf("\n[test_select_external]\n");

    // --- select on a single external channel ---
    {
        auto* ch      = makeChannel<int>(4, 4);
        auto* results = makeChannel<int>(4);

        coro([ch, results]() {
            exec_thread([ch](auto wake) {
                this_thread::sleep_for(chrono::milliseconds(10));
                while(!ch->sendExternalNoBlock(99)) {}
                wake();
            });
            int val = -1;
            select(Recv(ch, [&](int v){ val = v; }));
            results->send(val);
            results->close();
            ch->close();
        });

        scheduler_start();

        auto [v, err] = results->tryReceive();
        check(!err && v == 99, "select on single external channel: received correct value");
        delete ch;
        delete results;
    }

    // --- select mixing regular and external channels ---
    {
        auto* regular = makeChannel<int>(4);
        auto* ext     = makeChannel<int>(4, 4);
        auto* results = makeChannel<int>(4);

        coro([regular, ext, results]() {
            exec_thread([ext](auto wake) {
                this_thread::sleep_for(chrono::milliseconds(10));
                while(!ext->sendExternalNoBlock(7)) {}
                wake();
            });
            int got = -1;
            int idx = select(
                Recv(regular, [&](int v){ got = v * 100; }),
                Recv(ext,     [&](int v){ got = v; })
            );
            results->send(got);
            results->send(idx);
            results->close();
            regular->close();
            ext->close();
        });

        scheduler_start();

        auto [val,  e1] = results->tryReceive();
        auto [idx2, e2] = results->tryReceive();
        check(!e1 && val  == 7, "mixed select: received from external channel");
        check(!e2 && idx2 == 1, "mixed select: returned correct case index (1)");
        delete regular;
        delete ext;
        delete results;
    }

    // --- select with Default case on empty external channel ---
    {
        auto* ext     = makeChannel<int>(4, 4);
        auto* results = makeChannel<int>(4);

        coro([ext, results]() {
            int got = -1;
            int idx = select(
                Recv(ext,  [&](int v){ got = v; }),
                Default([&]{ got = 0; })
            );
            results->send(got);
            results->send(idx);
            results->close();
            ext->close();
        });

        scheduler_start();

        auto [val,  e1] = results->tryReceive();
        auto [idx2, e2] = results->tryReceive();
        check(!e1 && val  == 0,              "select+default on empty ext channel: default fires");
        check(!e2 && idx2 == SELECT_DEFAULT, "select+default: returned SELECT_DEFAULT");
        delete ext;
        delete results;
    }
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    test_simple_coroutine();
    test_channel();
    test_external_thread();
    test_wait_file();
    test_coro_sleep_wake();
    test_select_external();

    printf("\n──────────────────────────────\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
