// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------

#if defined(RASPBERRYPI_PICO) || defined(PICO_PLATFORM) || defined(LIB_PICO_PLATFORM)
    #define COROCGO_PLATFORM_PICO
#elif defined(_WIN32)
    #define COROCGO_PLATFORM_WINDOWS
#else
    #define COROCGO_PLATFORM_POSIX
#endif

// ---------------------------------------------------------------------------
// Feature flags  (can be overridden by the user before including this header)
// COROCGO_HAS_THREADS — enables ThreadPool, PollThread, exec_thread(), wait_file()
// COROCGO_HAS_FILE_IO — enables poll/pipe/fcntl based I/O waiting
// ---------------------------------------------------------------------------

#if defined(COROCGO_PLATFORM_PICO)
    #ifndef COROCGO_HAS_THREADS
        #define COROCGO_HAS_THREADS 0
    #endif
    #ifndef COROCGO_HAS_FILE_IO
        #define COROCGO_HAS_FILE_IO 0
    #endif
#elif defined(COROCGO_PLATFORM_WINDOWS)
    #ifndef COROCGO_HAS_THREADS
        #define COROCGO_HAS_THREADS 1
    #endif
    #ifndef COROCGO_HAS_FILE_IO
        #define COROCGO_HAS_FILE_IO 0  // Windows has no poll/epoll; use WaitForMultipleObjects or IOCP instead
    #endif
#else
    #ifndef COROCGO_HAS_THREADS
        #define COROCGO_HAS_THREADS 1
    #endif
    #ifndef COROCGO_HAS_FILE_IO
        #define COROCGO_HAS_FILE_IO 1
    #endif
#endif

#ifndef goengine_h
#define goengine_h

// ---------------------------------------------------------------------------
// Synchronization primitives abstraction
// ---------------------------------------------------------------------------

#if COROCGO_HAS_THREADS

#include <mutex>
#include <condition_variable>

namespace corocgo {
    using coro_mutex_t      = std::mutex;
    using coro_cond_var_t   = std::condition_variable;

    template<class M> using coro_lock_guard_t  = std::lock_guard<M>;
    template<class M> using coro_unique_lock_t = std::unique_lock<M>;
}

#else // no threads

namespace corocgo {

    struct coro_mutex_t {
        void lock()   {}
        void unlock() {}
    };

    template<class M>
    struct coro_unique_lock_t {
        explicit coro_unique_lock_t(M&) {}
    };

    template<class M>
    struct coro_lock_guard_t {
        explicit coro_lock_guard_t(M&) {}
    };

    // No-op condition variable — on single-threaded platforms the scheduler
    // never sleeps waiting for another thread to wake it.
    struct coro_cond_var_t {
        void notify_one() {}
        void notify_all() {}

        template<class L, class Predicate>
        void wait(L&, Predicate) {}

        template<class L, class Duration, class Predicate>
        bool wait_for(L&, const Duration&, Predicate pred) {
            return pred();
        }
    };

} // namespace corocgo

#endif // COROCGO_HAS_THREADS

#include <functional>
#include <utility>
#include <atomic>
#include <cassert>
#include <tuple>
#include <type_traits>

namespace corocgo {

enum WAIT_MODE { WAIT_IN=1, WAIT_OUT=2 };
enum SchedulerResult { SUCCESS, DEADLOCK };

void coro(std::function<void()>runnable);
SchedulerResult scheduler_start();
void scheduler_init();
bool scheduler_step();
void scheduler_stop();
void coro_yield();
void sleep(int milliseconds);

struct CoroSleepHandler {
    void* _cor;
    void sleep() const;
    void wake() const;
};
CoroSleepHandler coro_sleep_wake();

void exec_thread(std::function<void(std::function<void()>)> future);
std::pair<int,int> wait_file(int fd, int modeBitFlag);

// internal monitor bridge (used by Channel template)
void* _monitor_create();
void _monitor_destroy(void* monitor);
void _monitor_wait(void* monitor);
void _monitor_wake(void* monitor);
void _monitor_wake_all(void* monitor);

// thread-safe monitor bridge (used by Channel with external send)
void* _monitor_ts_create();
void _monitor_ts_destroy(void* monitor);
void _monitor_ts_wait(void* monitor);
void _monitor_ts_wake(void* monitor);
void _monitor_ts_wake_external(void* monitor);
void _monitor_ts_wake_all(void* monitor);

// select support
void _monitor_add_waiter(void* monitor, void* cell);
void _monitor_remove_waiter(void* monitor, void* cell);
void _select_wait(void** monitors, int count);

template<typename T>
struct ChannelResult {
    T value;
    bool error;
};

template<typename T>
class Channel {
    T* buffer;
    int bufferSize;
    int count;
    int readIdx;
    int writeIdx;
    std::atomic<bool> _closed;
    void* sendMonitor;
    void* recvMonitor;
    bool _extEnabled;

    // External send buffer (SPSC: IRQ is sole producer, main is sole consumer)
    T* extBuffer;
    int extBufSize;
    std::atomic<int> extWriteIdx;  // producer (IRQ) writes, consumer reads
    std::atomic<int> extReadIdx;   // consumer (main) writes, producer reads

    void drainExternal() {
        int w = extWriteIdx.load(std::memory_order_acquire);
        while(extReadIdx.load(std::memory_order_relaxed) != w && count < bufferSize) {
            int r = extReadIdx.load(std::memory_order_relaxed);
            buffer[writeIdx]=std::move(extBuffer[r]);
            writeIdx=(writeIdx+1)%bufferSize;
            count++;
            extReadIdx.store((r+1)%extBufSize, std::memory_order_release);
        }
    }

public:
    Channel(int size, int extSize=0) {
        bufferSize=size;
        buffer=new T[size];
        count=0;
        readIdx=0;
        writeIdx=0;
        _closed.store(false,std::memory_order_relaxed);
        _extEnabled=(extSize>0);
        extBufSize=extSize;
        extWriteIdx.store(0,std::memory_order_relaxed);
        extReadIdx.store(0,std::memory_order_relaxed);
        if(_extEnabled) {
            extBuffer=new T[extSize];
            sendMonitor=_monitor_ts_create();
            recvMonitor=_monitor_ts_create();
        } else {
            extBuffer=nullptr;
            sendMonitor=_monitor_create();
            recvMonitor=_monitor_create();
        }
    }
    ~Channel() {
        if(_extEnabled) {
            _monitor_ts_destroy(sendMonitor);
            _monitor_ts_destroy(recvMonitor);
            delete[] extBuffer;
        } else {
            _monitor_destroy(sendMonitor);
            _monitor_destroy(recvMonitor);
        }
        delete[] buffer;
    }
    bool send(const T& value) {
        if(_closed.load(std::memory_order_relaxed)) return false;
        while (count>=bufferSize && !_closed.load(std::memory_order_relaxed)) {
            if (_extEnabled) {
                _monitor_ts_wait(sendMonitor);
            } else {
                _monitor_wait(sendMonitor);
            }
        }
        if(_closed.load(std::memory_order_relaxed)) return false;
        buffer[writeIdx]=value;
        writeIdx=(writeIdx+1)%bufferSize;
        count++;
        if(_extEnabled) _monitor_ts_wake(recvMonitor);
        else _monitor_wake(recvMonitor);
        return true;
    }
    ChannelResult<T> receive() {
        while(true) {
            if(count>0) {
                T value=buffer[readIdx];
                readIdx=(readIdx+1)%bufferSize;
                count--;
                if(_extEnabled) _monitor_ts_wake(sendMonitor);
                else _monitor_wake(sendMonitor);
                return {value, false};
            }
            if(_extEnabled && extWriteIdx.load(std::memory_order_acquire)!=extReadIdx.load(std::memory_order_relaxed)) {
                drainExternal();
                continue;
            }
            if(_closed.load(std::memory_order_relaxed)) {
                return {T{}, true};
            }
            if(_extEnabled) _monitor_ts_wait(recvMonitor);
            else _monitor_wait(recvMonitor);
        }
    }
    // Thread-safe non-blocking send from external (non-coroutine) thread.
    // Returns false if buffer is full, channel is closed, or external send not enabled.
    // Channel must be created with extSize > 0.
    bool sendExternalNoBlock(T value) {
        if(!_extEnabled) return false;
        if(_closed.load(std::memory_order_acquire)) return false;
        int w = extWriteIdx.load(std::memory_order_relaxed);
        int next = (w+1)%extBufSize;
        if(next == extReadIdx.load(std::memory_order_acquire)) return false; // full
        extBuffer[w] = value;
        extWriteIdx.store(next, std::memory_order_release);
        _monitor_ts_wake_external(recvMonitor);
        return true;
    }
    // Safe to call from external thread if _extEnabled.
    void close() {
        _closed.store(true,std::memory_order_release);
        if (_extEnabled) {
            _monitor_ts_wake_all(sendMonitor);
            _monitor_ts_wake_all(recvMonitor);
        } else {
            _monitor_wake_all(sendMonitor);
            _monitor_wake_all(recvMonitor);
        }
    }
    bool isClosed() {
        return _closed.load(std::memory_order_relaxed);
    }
    ChannelResult<T> tryReceive() {
        if(_extEnabled && extWriteIdx.load(std::memory_order_acquire)!=extReadIdx.load(std::memory_order_relaxed)) {
            drainExternal();
        }
        if(count>0) {
            T value=buffer[readIdx];
            readIdx=(readIdx+1)%bufferSize;
            count--;
            if(_extEnabled) _monitor_ts_wake(sendMonitor);
            else _monitor_wake(sendMonitor);
            return {value, false};
        }
        return {T{}, true};
    }
    void* getRecvMonitor() { return recvMonitor; }
    bool isExtEnabled() { return _extEnabled; }
};

template<typename T>
Channel<T>* makeChannel(int size=1, int extSize=0) {
    return new Channel<T>(size, extSize);
}

// ── select ──

enum SelectResult {
    SELECT_CLOSED  = -1,   // all channels closed
    SELECT_DEFAULT = -2,   // default case executed
    // >= 0: index of the RecvCase that fired
};

template<typename T>
struct RecvCase {
    Channel<T>* channel;
    std::function<void(T)> handler;
};

template<typename T, typename F>
RecvCase<T> Recv(Channel<T>* ch, F handler) {
    return {ch, std::function<void(T)>(handler)};
}

struct DefaultCase {
    std::function<void()> handler;
};

inline DefaultCase Default(std::function<void()> handler) {
    return {handler};
}

namespace _select_detail {

// Get the last type in a parameter pack
template<typename... Ts> struct LastType;
template<typename T> struct LastType<T> { using type=T; };
template<typename T, typename... Rest> struct LastType<T, Rest...> : LastType<Rest...> {};

template<typename... Cases>
struct HasDefault {
    static constexpr bool value=std::is_same<typename LastType<Cases...>::type, DefaultCase>::value;
};

// Try non-blocking receive on all RecvCases. Returns index of first success, -1 if none.
template<typename Case>
int tryOne(Case& c, int idx) {
    auto r=c.channel->tryReceive();
    if(!r.error) {
        c.handler(r.value);
        return idx;
    }
    return -1;
}

inline int tryOne(DefaultCase&, int) { return -1; }

template<typename... Cases>
int tryAll(int idx, Cases&... cases);

inline int tryAll(int) { return -1; }

template<typename First, typename... Rest>
int tryAll(int idx, First& first, Rest&... rest) {
    int r=tryOne(first, idx);
    if(r>=0) return r;
    return tryAll(idx+1, rest...);
}

// Check if all RecvCase channels are closed
template<typename Case>
bool isCaseClosed(Case& c) { return c.channel->isClosed(); }
inline bool isCaseClosed(DefaultCase&) { return true; }

template<typename... Cases>
bool allClosed(Cases&... cases);

inline bool allClosed() { return true; }

template<typename First, typename... Rest>
bool allClosed(First& first, Rest&... rest) {
    return isCaseClosed(first) && allClosed(rest...);
}

// Collect monitors from RecvCases (skip DefaultCase)
template<typename Case>
void collectMonitor(Case& c, void** monitors, int& idx) {
    assert(!c.channel->isExtEnabled() &&
        "select does not support channels with external send (extSize>0)");
    monitors[idx++]=c.channel->getRecvMonitor();
}
inline void collectMonitor(DefaultCase&, void**, int&) {}

template<typename... Cases>
void collectMonitors(void** monitors, int& idx, Cases&... cases);

inline void collectMonitors(void**, int&) {}

template<typename First, typename... Rest>
void collectMonitors(void** monitors, int& idx, First& first, Rest&... rest) {
    collectMonitor(first, monitors, idx);
    collectMonitors(monitors, idx, rest...);
}

// Count RecvCases (exclude DefaultCase)
template<typename T> struct IsDefault { static constexpr bool value=false; };
template<> struct IsDefault<DefaultCase> { static constexpr bool value=true; };

template<typename... Cases>
constexpr int countRecvCases() {
    return (... + (IsDefault<Cases>::value ? 0 : 1));
}

} // namespace _select_detail

// select: wait on multiple channels, return index of first ready case.
// Returns SELECT_CLOSED if all channels closed.
// Returns SELECT_DEFAULT if Default case executed.
template<typename Last>
int select(Last last) {
    if constexpr(_select_detail::IsDefault<Last>::value) {
        last.handler();
        return SELECT_DEFAULT;
    } else {
        while(true) {
            auto r=last.channel->tryReceive();
            if(!r.error) { last.handler(r.value); return 0; }
            if(last.channel->isClosed()) return SELECT_CLOSED;
            void* mon=last.channel->getRecvMonitor();
            _select_wait(&mon, 1);
        }
    }
}

template<typename... Cases>
int select(Cases... cases) {
    constexpr int N=_select_detail::countRecvCases<Cases...>();
    constexpr bool hasDefault=_select_detail::HasDefault<Cases...>::value;

    while(true) {
        int result=_select_detail::tryAll(0, cases...);
        if(result>=0) return result;

        if(_select_detail::allClosed(cases...)) return SELECT_CLOSED;

        if constexpr(hasDefault) {
            auto tup=std::tie(cases...);
            auto& def=std::get<sizeof...(Cases)-1>(tup);
            def.handler();
            return SELECT_DEFAULT;
        }

        void* monitors[N];
        int mIdx=0;
        _select_detail::collectMonitors(monitors, mIdx, cases...);
        _select_wait(monitors, N);
    }
}

} // namespace corocgo

#endif
