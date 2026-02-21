#define MINICORO_IMPL
#include "minicoro.h"
#include "corocgo.h"
#include <chrono>
#include <atomic>
#include <vector>
#if COROCGO_HAS_THREADS
#include <thread>
#endif
#if COROCGO_HAS_FILE_IO
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

using namespace std;
using corocgo::coro_mutex_t;
using corocgo::coro_cond_var_t;
using corocgo::coro_lock_guard_t;
using corocgo::coro_unique_lock_t;

template<typename T> class BiLinkedList;
template<typename T>
struct BiLinkedCell {
    BiLinkedCell*previous=NULL;
    BiLinkedCell*next=NULL;
    BiLinkedList<T>*list=NULL;
    T data;
};

template<typename T>
class BiLinkedList {
private:
    BiLinkedCell<T>*head;
    BiLinkedCell<T>*tail;
    int _size;
public:
    BiLinkedList() {
        head=NULL;
        tail=NULL;
        _size=0;
    }
    BiLinkedCell<T>*newCell() {
        return new BiLinkedCell<T>();
    }
    void deleteCell(BiLinkedCell<T>*cell) {
        delete cell;
    }
    BiLinkedCell<T>* append(T data) {
        BiLinkedCell<T>*cell=newCell();
        cell->data=data;
        return append(cell);
    }
    BiLinkedCell<T>* append(BiLinkedCell<T>*cell) {
        if (cell->list==this) return cell;
        _size++;
        cell->next=NULL;
        cell->previous=NULL;
        cell->list=this;
        if (head==NULL) {
            head=cell;
            tail=cell;
            return cell;
        }
        tail->next=cell;
        cell->previous=tail;
        tail=cell;
        return cell;
    }
    BiLinkedCell<T>*appendFront(BiLinkedCell<T>*cell) {
        if (cell->list==this) return cell;
        _size++;
        cell->next=NULL;
        cell->previous=NULL;
        cell->list=this;
        if (head==NULL) {
            head=cell;
            tail=cell;
            return cell;
        }
        cell->next=head;
        head->previous=cell;
        head=cell;
        return cell;
    }
    BiLinkedCell<T>*insertBefore(BiLinkedCell<T>*cell, BiLinkedCell<T>*before) {
        if(cell->list==this) return cell;
        if(before==NULL) return append(cell);
        if(before==head) return appendFront(cell);
        _size++;
        cell->list=this;
        cell->next=before;
        cell->previous=before->previous;
        before->previous->next=cell;
        before->previous=cell;
        return cell;
    }
    bool remove(BiLinkedCell<T>*cell) {
        if (cell->list!=this) return false;
        if (head==NULL) return false;
        if (cell==head) {
            if (cell==tail) {
                head=NULL;
                tail=NULL;
            } else {
                head=head->next;
                head->previous=NULL;
            }
        } else if(cell==tail) {
            tail=tail->previous;
            tail->next=NULL;
        } else {
            BiLinkedCell<T>*prev=cell->previous;
            BiLinkedCell<T>*next=cell->next;
            prev->next=next;
            next->previous=prev;
        }
        cell->previous=NULL;
        cell->next=NULL;
        cell->list=NULL;
        _size--;
        return true;
    }
    int size() {
        return _size;
    }
    BiLinkedCell<T>*peekFront() {
        return head;
    }
    BiLinkedCell<T>*peekBack() {
        return tail;
    }
    BiLinkedCell<T>*next(BiLinkedCell<T>*cell) {
        if(cell->list!=this)return NULL;
        return cell->next;
    }
    BiLinkedCell<T>*previous(BiLinkedCell<T>*cell) {
        if(cell->list!=this)return NULL;
        return cell->previous;
    }
    BiLinkedCell<T>*search(T data) {
        BiLinkedCell<T>*cell=head;
        while(cell!=NULL) {
            if(cell->data==data) return cell;
            cell=cell->next;
        }
        return NULL;
    }
    BiLinkedCell<T>*removeBack() {
        if(tail==NULL) return NULL;
        BiLinkedCell<T>*cell=tail;
        remove(cell);
        return cell;
    }
    BiLinkedCell<T>*removeFront() {
        if(head==NULL) return NULL;
        BiLinkedCell<T>*cell=head;
        remove(cell);
        return cell;
    }
};

// ── Thread pool ──

#if COROCGO_HAS_THREADS
class ThreadPool {
    vector<thread> workers;
    static constexpr int QUEUE_CAP=256;
    function<void()> taskBuf[QUEUE_CAP];
    int qHead=0,qTail=0,qCount=0;
    mutex mtx;
    condition_variable cv;
    condition_variable cvNotFull;
    bool stopped=false;
public:
    ThreadPool(int numThreads) {
        for(int i=0;i<numThreads;i++) {
            workers.emplace_back([this]() {
                while(true) {
                    function<void()> task;
                    {
                        unique_lock<mutex> lock(mtx);
                        cv.wait(lock,[this]() { return stopped||qCount>0; });
                        if(stopped&&qCount==0) return;
                        task=std::move(taskBuf[qHead]);
                        taskBuf[qHead]=nullptr;
                        qHead=(qHead+1)%QUEUE_CAP;
                        qCount--;
                    }
                    cvNotFull.notify_one();
                    task();
                }
            });
        }
    }
    void submit(function<void()> task) {
        {
            unique_lock<mutex> lock(mtx);
            cvNotFull.wait(lock,[this]() { return qCount<QUEUE_CAP||stopped; });
            if(stopped) return;
            taskBuf[qTail]=std::move(task);
            qTail=(qTail+1)%QUEUE_CAP;
            qCount++;
        }
        cv.notify_one();
    }
    ~ThreadPool() {
        {
            lock_guard<mutex> lock(mtx);
            stopped=true;
        }
        cv.notify_all();
        cvNotFull.notify_all();
        for(auto& w:workers) w.join();
    }
};
#endif // COROCGO_HAS_THREADS

// ── Coroutine internals ──

class Coroutine{
public:
    function<void()>runnable;
    mco_coro* coroutine=nullptr;
    BiLinkedCell<Coroutine*>*cell=nullptr;
    chrono::steady_clock::time_point wakeUpTime;
    int fdWaitResult=0;
    int fdWaitError=0;
    bool inSelect=false;
    Coroutine* nextFree=nullptr;
};

BiLinkedList<Coroutine*> mainCoroutinesQueue;
BiLinkedList<Coroutine*> sleepQueue;
int totalCoroutines=0;

// ── Object pools (intrusive free-lists) ──

Coroutine* freeCoroutineHead=nullptr;
BiLinkedCell<Coroutine*>* freeCellHead=nullptr;

Coroutine* acquireCoroutine() {
    if(freeCoroutineHead) {
        Coroutine* c=freeCoroutineHead;
        freeCoroutineHead=c->nextFree;
        c->nextFree=nullptr;
        return c;
    }
    return new Coroutine();
}

void releaseCoroutine(Coroutine* c) {
    c->runnable=nullptr;
    c->coroutine=nullptr;
    c->cell=nullptr;
    c->fdWaitResult=0;
    c->fdWaitError=0;
    c->inSelect=false;
    c->nextFree=freeCoroutineHead;
    freeCoroutineHead=c;
}

BiLinkedCell<Coroutine*>* acquireCell() {
    if(freeCellHead) {
        BiLinkedCell<Coroutine*>* c=freeCellHead;
        freeCellHead=c->next;
        c->next=nullptr;
        c->previous=nullptr;
        c->list=nullptr;
        return c;
    }
    return new BiLinkedCell<Coroutine*>();
}

void releaseCell(BiLinkedCell<Coroutine*>* cell) {
    cell->data=nullptr;
    cell->previous=nullptr;
    cell->list=nullptr;
    cell->next=freeCellHead;
    freeCellHead=cell;
}

// ── Thread-safe wake infrastructure ──
coro_mutex_t pendingWakeMtx;
BiLinkedList<Coroutine*> pendingWakeQueue;
int threadWaitCount=0;
#if COROCGO_HAS_THREADS
ThreadPool* globalThreadPool=nullptr;
#endif

coro_cond_var_t schedulerCV;
#if COROCGO_HAS_FILE_IO
class PollThread;
PollThread* globalPollThread=nullptr;
#endif

// ── WaitQueue (internal coroutine condition variable) ──

class WaitQueue {
    BiLinkedList<Coroutine*> waitQueue;
    bool threadSafe=false;
    BiLinkedCell<Coroutine*>* threadSafeCell=nullptr;
public:
    void wait() {
        Coroutine*cor=(Coroutine*)mco_running()->user_data;
        mainCoroutinesQueue.remove(cor->cell);
        waitQueue.append(cor->cell);
        mco_yield(mco_running());
    }
    void wake() {
        if(threadSafe) {
            BiLinkedCell<Coroutine*>*cell=threadSafeCell;
            if(cell==nullptr) return;
            threadSafeCell=nullptr;
            {
                coro_lock_guard_t<coro_mutex_t> lock(pendingWakeMtx);
                pendingWakeQueue.append(cell);
                threadWaitCount--;
            }
            schedulerCV.notify_one();
            return;
        }
        BiLinkedCell<Coroutine*>*cell=waitQueue.removeFront();
        if(cell==NULL) return;
        Coroutine*cor=cell->data;
        if(cor->inSelect) {
            mainCoroutinesQueue.append(cor->cell);
        } else {
            mainCoroutinesQueue.append(cell);
        }
    }
    void wakeAll() {
        BiLinkedCell<Coroutine*>*cell=waitQueue.removeFront();
        while(cell!=NULL) {
            Coroutine*cor=cell->data;
            if(cor->inSelect) {
                mainCoroutinesQueue.append(cor->cell);
            } else {
                mainCoroutinesQueue.append(cell);
            }
            cell=waitQueue.removeFront();
        }
    }
    void addWaiter(BiLinkedCell<Coroutine*>*cell) {
        waitQueue.append(cell);
    }
    void removeWaiter(BiLinkedCell<Coroutine*>*cell) {
        waitQueue.remove(cell);
    }
    void setThreadSafe(BiLinkedCell<Coroutine*>*cell) {
        threadSafe=true;
        threadSafeCell=cell;
    }
};

// ── TSWaitQueue (thread-safe coroutine condition variable) ──

class TSWaitQueue {
    BiLinkedList<Coroutine*> waitQueue;
    coro_mutex_t mtx;
    std::atomic<int> waitCount{0};
public:
    void wait() {
#if COROCGO_HAS_THREADS
        Coroutine*cor=(Coroutine*)mco_running()->user_data;
        mainCoroutinesQueue.remove(cor->cell);
        {
            coro_lock_guard_t<coro_mutex_t> lock(mtx);
            waitQueue.append(cor->cell);
            waitCount.fetch_add(1,std::memory_order_release);
        }
        {
            coro_lock_guard_t<coro_mutex_t> lock(pendingWakeMtx);
            threadWaitCount++;
        }
        mco_yield(mco_running());
#else
        // Pico: cooperative polling — stay in mainCoroutinesQueue, just yield.
        // The caller (e.g. receive()) will re-check the channel on the next scheduler step.
        mco_yield(mco_running());
#endif
    }
    void wake() {
#if COROCGO_HAS_THREADS
        if(waitCount.load(std::memory_order_acquire)==0) return;
        BiLinkedCell<Coroutine*>*cell;
        {
            coro_lock_guard_t<coro_mutex_t> lock(mtx);
            cell=waitQueue.removeFront();
            if(cell==NULL) return;
            waitCount.fetch_sub(1,std::memory_order_release);
        }
        mainCoroutinesQueue.append(cell);
        {
            coro_lock_guard_t<coro_mutex_t> lock(pendingWakeMtx);
            threadWaitCount--;
        }
#endif
        // Pico: no-op — coroutine was never removed from mainCoroutinesQueue
    }
    void wakeExternal() {
#if COROCGO_HAS_THREADS
        if(waitCount.load(std::memory_order_acquire)==0) return;
        BiLinkedCell<Coroutine*>*cell;
        {
            coro_lock_guard_t<coro_mutex_t> lock(mtx);
            cell=waitQueue.removeFront();
            if(cell==NULL) return;
            waitCount.fetch_sub(1,std::memory_order_release);
        }
        {
            coro_lock_guard_t<coro_mutex_t> lock(pendingWakeMtx);
            pendingWakeQueue.append(cell);
            threadWaitCount--;
        }
        schedulerCV.notify_one();
#endif
        // Pico: no-op — coroutine polls via yield, no linked-list touched from IRQ
    }
    void wakeAll() {
#if COROCGO_HAS_THREADS
        if(waitCount.load(std::memory_order_acquire)==0) return;
        {
            coro_lock_guard_t<coro_mutex_t> lock(mtx);
            BiLinkedCell<Coroutine*>*cell=waitQueue.removeFront();
            if(cell==NULL) return;
            {
                coro_lock_guard_t<coro_mutex_t> lock2(pendingWakeMtx);
                while(cell!=NULL) {
                    pendingWakeQueue.append(cell);
                    threadWaitCount--;
                    cell=waitQueue.removeFront();
                }
            }
            waitCount.store(0,std::memory_order_release);
        }
        schedulerCV.notify_one();
#endif
        // Pico: no-op — coroutines poll via yield
    }
};

// ── Poll thread ──

#if COROCGO_HAS_FILE_IO
class PollThread {
    struct Registration {
        int fd;
        short events;
        Coroutine* cor;
        WaitQueue* monitor;
    };
    thread worker;
    mutex regMtx;
    vector<Registration> pendingRegs;
    int wakePipe[2]={-1,-1};
    bool stopped=false;
    vector<struct pollfd> pollFds;
    vector<Registration> activeRegs;
public:
    PollThread() {
        pipe(wakePipe);
        fcntl(wakePipe[0],F_SETFL,O_NONBLOCK);
        fcntl(wakePipe[1],F_SETFL,O_NONBLOCK);
        pollFds.reserve(64);
        activeRegs.reserve(64);
        pendingRegs.reserve(32);
        struct pollfd pfd;
        pfd.fd=wakePipe[0];
        pfd.events=POLLIN;
        pfd.revents=0;
        pollFds.push_back(pfd);
        activeRegs.push_back({});
        worker=thread([this]() { run(); });
    }
    void addFd(int fd,short events,Coroutine*cor,WaitQueue*monitor) {
        {
            lock_guard<mutex> lock(regMtx);
            pendingRegs.push_back({fd,events,cor,monitor});
        }
        char buf=1;
        write(wakePipe[1],&buf,1);
    }
    void stop() {
        {
            lock_guard<mutex> lock(regMtx);
            stopped=true;
        }
        char buf=1;
        write(wakePipe[1],&buf,1);
        worker.join();
        close(wakePipe[0]);
        close(wakePipe[1]);
    }
private:
    void run() {
        while(true) {
            poll(pollFds.data(),(nfds_t)pollFds.size(),-1);
            if(pollFds[0].revents&POLLIN) {
                char drain[256];
                while(read(wakePipe[0],drain,sizeof(drain))>0) {}
                lock_guard<mutex> lock(regMtx);
                if(stopped) return;
                for(auto& r:pendingRegs) {
                    struct pollfd pfd;
                    pfd.fd=r.fd;
                    pfd.events=r.events;
                    pfd.revents=0;
                    pollFds.push_back(pfd);
                    activeRegs.push_back(r);
                }
                pendingRegs.clear();
            }
            for(int i=(int)pollFds.size()-1;i>=1;i--) {
                if(pollFds[i].revents==0) continue;
                auto& reg=activeRegs[i];
                int result=0;
                int error=0;
                if(pollFds[i].revents&POLLIN)  result|=corocgo::WAIT_IN;
                if(pollFds[i].revents&POLLOUT) result|=corocgo::WAIT_OUT;
                if(pollFds[i].revents&(POLLERR|POLLNVAL)) error=EIO;
                if((pollFds[i].revents&POLLHUP) && result==0) error=EPIPE;
                reg.cor->fdWaitResult=result;
                reg.cor->fdWaitError=error;
                pollFds[i]=pollFds.back();
                activeRegs[i]=activeRegs.back();
                pollFds.pop_back();
                activeRegs.pop_back();
                reg.monitor->wake();
            }
        }
    }
};
#endif // COROCGO_HAS_FILE_IO

// ── Public API ──

namespace corocgo {

// ── Monitor bridge functions ──

void* _monitor_create() {
    return new WaitQueue();
}

void _monitor_destroy(void* monitor) {
    delete (WaitQueue*)monitor;
}

void _monitor_wait(void* monitor) {
    ((WaitQueue*)monitor)->wait();
}

void _monitor_wake(void* monitor) {
    ((WaitQueue*)monitor)->wake();
}

void _monitor_wake_all(void* monitor) {
    ((WaitQueue*)monitor)->wakeAll();
}

void _monitor_add_waiter(void* monitor, void* cell) {
    ((WaitQueue*)monitor)->addWaiter((BiLinkedCell<Coroutine*>*)cell);
}

void _monitor_remove_waiter(void* monitor, void* cell) {
    ((WaitQueue*)monitor)->removeWaiter((BiLinkedCell<Coroutine*>*)cell);
}

// ── Select wait ──

void _select_wait(void** monitors, int count) {
    mco_coro* co=mco_running();
    Coroutine* cor=(Coroutine*)co->user_data;

    BiLinkedCell<Coroutine*>** tempCells=
        (BiLinkedCell<Coroutine*>**)alloca(count*sizeof(BiLinkedCell<Coroutine*>*));
    for(int i=0;i<count;i++) {
        tempCells[i]=acquireCell();
        tempCells[i]->data=cor;
    }

    cor->inSelect=true;
    mainCoroutinesQueue.remove(cor->cell);

    for(int i=0;i<count;i++) {
        ((WaitQueue*)monitors[i])->addWaiter(tempCells[i]);
    }

    mco_yield(co);

    cor->inSelect=false;
    for(int i=0;i<count;i++) {
        if(tempCells[i]->list!=nullptr) {
            ((WaitQueue*)monitors[i])->removeWaiter(tempCells[i]);
        }
        releaseCell(tempCells[i]);
    }
    // cor->cell is already in mainCoroutinesQueue (put there by modified wake())
}

// ── Thread-safe monitor bridge functions ──

void* _monitor_ts_create() {
    return new TSWaitQueue();
}

void _monitor_ts_destroy(void* monitor) {
    delete (TSWaitQueue*)monitor;
}

void _monitor_ts_wait(void* monitor) {
    ((TSWaitQueue*)monitor)->wait();
}

void _monitor_ts_wake(void* monitor) {
    ((TSWaitQueue*)monitor)->wake();
}

void _monitor_ts_wake_external(void* monitor) {
    ((TSWaitQueue*)monitor)->wakeExternal();
}

void _monitor_ts_wake_all(void* monitor) {
    ((TSWaitQueue*)monitor)->wakeAll();
}

// ── Fd wait ──

#if COROCGO_HAS_FILE_IO
pair<int,int> wait_file(int fd, int modeBitFlag) {
    mco_coro*co=mco_running();
    Coroutine*cor=(Coroutine*)co->user_data;
    cor->fdWaitResult=0;
    cor->fdWaitError=0;
    short events=0;
    if(modeBitFlag&WAIT_IN)  events|=POLLIN;
    if(modeBitFlag&WAIT_OUT) events|=POLLOUT;
    if(events==0) return {0,EINVAL};
    WaitQueue monitor;
    monitor.setThreadSafe(cor->cell);
    mainCoroutinesQueue.remove(cor->cell);
    {
        coro_lock_guard_t<coro_mutex_t> lock(pendingWakeMtx);
        threadWaitCount++;
    }
    globalPollThread->addFd(fd,events,cor,&monitor);
    mco_yield(co);
    return {cor->fdWaitResult,cor->fdWaitError};
}
#endif // COROCGO_HAS_FILE_IO

// ── Thread-safe exec ──

void Monitor::wake() {
    ((WaitQueue*)monitor)->wake();
}

#if COROCGO_HAS_THREADS
void exec_thread(function<void(Monitor)> future) {
    mco_coro* co=mco_running();
    Coroutine* cor=(Coroutine*)co->user_data;

    WaitQueue wq;
    wq.setThreadSafe(cor->cell);
    mainCoroutinesQueue.remove(cor->cell);
    {
        coro_lock_guard_t<coro_mutex_t> lock(pendingWakeMtx);
        threadWaitCount++;
    }

    Monitor mon((void*)&wq);
    auto* futurePtr=&future;
    globalThreadPool->submit([futurePtr,mon]() {
        (*futurePtr)(mon);
    });

    mco_yield(co);
}
#endif // COROCGO_HAS_THREADS

// ── Coroutine engine ──

static void coroutine_entry(mco_coro* co) {
    Coroutine*cor;
    mco_pop(co, &cor, sizeof(cor));
    cor->runnable();
}

void coro(function<void()>runnable) {
    mco_desc desc = mco_desc_init(coroutine_entry, 0);
    mco_coro* co;
    mco_create(&co, &desc);

    BiLinkedCell<Coroutine*>*cell = acquireCell();
    Coroutine*cor=acquireCoroutine();
    cor->runnable = std::move(runnable);
    cor->coroutine = co;
    cor->cell = cell;
    cell->data=cor;
    mainCoroutinesQueue.append(cell);
    totalCoroutines++;

    co->user_data=cor;
    mco_push(co, &cor, sizeof(cor));
}

void coro_yield() {
    mco_yield(mco_running());
}

void sleep(int milliseconds) {
    mco_coro* co=mco_running();
    Coroutine* cor=(Coroutine*)co->user_data;
    cor->wakeUpTime=chrono::steady_clock::now()+chrono::milliseconds(milliseconds);
    mainCoroutinesQueue.remove(cor->cell);
    BiLinkedCell<Coroutine*>*pos=sleepQueue.peekFront();
    while(pos!=NULL && pos->data->wakeUpTime<=cor->wakeUpTime) {
        pos=pos->next;
    }
    sleepQueue.insertBefore(cor->cell, pos);
    mco_yield(co);
}

// ── Shared scheduler phase helpers ──

static void scheduler_drain_pending() {
    coro_lock_guard_t<coro_mutex_t> lock(pendingWakeMtx);
    BiLinkedCell<Coroutine*>*cell=pendingWakeQueue.removeFront();
    while(cell!=nullptr) {
        mainCoroutinesQueue.append(cell);
        cell=pendingWakeQueue.removeFront();
    }
}

static void scheduler_wake_sleepers() {
    auto now=chrono::steady_clock::now();
    while(true) {
        BiLinkedCell<Coroutine*>*front=sleepQueue.peekFront();
        if(front==NULL) break;
        if(front->data->wakeUpTime>now) break;
        sleepQueue.remove(front);
        mainCoroutinesQueue.append(front);
    }
}

static void scheduler_run_ready() {
    BiLinkedCell<Coroutine*>*cell=mainCoroutinesQueue.peekFront();
    while(cell!=NULL) {
        BiLinkedCell<Coroutine*>*next=cell->next;
        Coroutine*c=cell->data;
        mco_resume(c->coroutine);
        if(mco_status(c->coroutine)==MCO_DEAD) {
            mco_destroy(c->coroutine);
            mainCoroutinesQueue.remove(cell);
            releaseCell(cell);
            releaseCoroutine(c);
            totalCoroutines--;
        }
        cell=next;
    }
}

// ── Scheduler lifecycle ──

void scheduler_init() {
#if COROCGO_HAS_THREADS
    int poolSize=(int)thread::hardware_concurrency();
    if(poolSize<2) poolSize=2;
    globalThreadPool=new ThreadPool(poolSize);
#endif
#if COROCGO_HAS_FILE_IO
    globalPollThread=new PollThread();
#endif
}

bool scheduler_step() {
    scheduler_drain_pending();
    scheduler_wake_sleepers();
    if(mainCoroutinesQueue.size()>0) {
        scheduler_run_ready();
    }
    return totalCoroutines>0;
}

void scheduler_stop() {
#if COROCGO_HAS_FILE_IO
    if(globalPollThread) {
        globalPollThread->stop();
        delete globalPollThread;
        globalPollThread=nullptr;
    }
#endif
#if COROCGO_HAS_THREADS
    if(globalThreadPool) {
        delete globalThreadPool;
        globalThreadPool=nullptr;
    }
#endif
    while(freeCoroutineHead) {
        Coroutine* next=freeCoroutineHead->nextFree;
        delete freeCoroutineHead;
        freeCoroutineHead=next;
    }
    while(freeCellHead) {
        BiLinkedCell<Coroutine*>* next=freeCellHead->next;
        delete freeCellHead;
        freeCellHead=next;
    }
}

SchedulerResult scheduler_start() {
    scheduler_init();

    while(totalCoroutines>0) {
        scheduler_drain_pending();
        scheduler_wake_sleepers();

        // Phase 2: if nothing ready, block (unique to scheduler_start)
        if(mainCoroutinesQueue.size()==0) {
            coro_unique_lock_t<coro_mutex_t> lock(pendingWakeMtx);
            if(sleepQueue.size()>0) {
                auto dur=sleepQueue.peekFront()->data->wakeUpTime-chrono::steady_clock::now();
                auto ms=chrono::duration_cast<chrono::milliseconds>(dur).count();
                if(ms>0)
                    schedulerCV.wait_for(lock,chrono::milliseconds(ms),
                        []() { return pendingWakeQueue.size()>0; });
            } else if(threadWaitCount>0) {
                schedulerCV.wait(lock,
                    []() { return pendingWakeQueue.size()>0; });
            } else {
                scheduler_stop();
                return DEADLOCK;
            }
            continue;
        }

        scheduler_run_ready();
    }

    scheduler_stop();
    return SUCCESS;
}

} // namespace corocgo
