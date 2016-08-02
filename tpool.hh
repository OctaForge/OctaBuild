#ifndef OBUILD_TPOOL_HH
#define OBUILD_TPOOL_HH

#include <ostd/thread.hh>

using ostd::Thread;
using ostd::UniqueLock;
using ostd::Mutex;
using ostd::Condition;
using ostd::Vector;

struct ThreadPool {
    ThreadPool():
        cond(), mtx(), thrs(), tasks(nullptr), last_task(nullptr),
        running(false)
    {}

    ~ThreadPool() {
        destroy();
    }

    static void *thr_func(void *ptr) {
        static_cast<ThreadPool *>(ptr)->run();
        return nullptr;
    }

    bool init(ostd::Size size) {
        running = true;
        for (ostd::Size i = 0; i < size; ++i) {
            Thread tid([this]() { run(); });
            if (!tid) {
                return false;
            }
            thrs.push(ostd::move(tid));
        }
        return true;
    }

    void destroy() {
        mtx.lock();
        if (!running) {
            mtx.unlock();
            return;
        }
        running = false;
        mtx.unlock();
        cond.broadcast();
        for (Thread &tid: thrs.iter()) {
            tid.join();
            cond.broadcast();
        }
    }

    void run() {
        for (;;) {
            UniqueLock<Mutex> l(mtx);
            while (running && (tasks == nullptr)) {
                cond.wait(l);
            }
            if (!running) {
                l.unlock();
                ostd::this_thread::exit();
            }
            Task *t = tasks;
            tasks = t->next;
            if (last_task == t) {
                last_task = nullptr;
            }
            l.unlock();
            t->cb();
            delete t;
        }
    }

    void push(ostd::Function<void()> func) {
        mtx.lock();
        Task *t = new Task(ostd::move(func));
        if (last_task) {
            last_task->next = t;
        }
        last_task = t;
        if (!tasks) {
            tasks = t;
        }
        cond.signal();
        mtx.unlock();
    }

private:
    struct Task {
        ostd::Function<void()> cb;
        Task *next = nullptr;
        Task() = delete;
        Task(Task const &) = delete;
        Task(Task &&) = delete;
        Task(ostd::Function<void()> &&cbf): cb(ostd::move(cbf)) {}
        Task &operator=(Task const &) = delete;
        Task &operator=(Task &&) = delete;
    };

    Condition cond;
    Mutex mtx;
    Vector<Thread> thrs;
    Task *tasks;
    Task *last_task;
    bool volatile running;
};

#endif