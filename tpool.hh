#ifndef OCTABUILD_TPOOL_HH
#define OCTABUILD_TPOOL_HH

#include <pthread.h>

#include <ostd/types.hh>
#include <ostd/functional.hh>
#include <ostd/vector.hh>
#include <ostd/utility.hh>

struct Mutex {
    Mutex() {
        pthread_mutex_init(&mtx, nullptr);
        locked = false;
    }

    ~Mutex() {
        while (locked) unlock();
        pthread_mutex_destroy(&mtx);
    }

    void lock() {
        pthread_mutex_lock(&mtx);
        locked = true;
    }

    void unlock() {
        locked = false;
        pthread_mutex_unlock(&mtx);
    }

    pthread_mutex_t mtx;
    volatile bool locked;
};

struct Cond {
    Cond() {
        pthread_cond_init(&cnd, nullptr);
    }

    ~Cond() {
        pthread_cond_destroy(&cnd);
    }

    void signal() {
        pthread_cond_signal(&cnd);
    }

    void broadcast() {
        pthread_cond_broadcast(&cnd);
    }

    void wait(Mutex &m) {
        pthread_cond_wait(&cnd, &m.mtx);
    }

    pthread_cond_t cnd;
};

struct Task {
    ostd::Function<void()> cb;
    Task *next = nullptr;
    Task(ostd::Function<void()> &&cb): cb(ostd::move(cb)) {}
};

struct ThreadPool {
    ThreadPool() {}

    ~ThreadPool() {
        if (running) destroy();
    }

    static void *thr_func(void *ptr) {
        ((ThreadPool *)ptr)->run();
        return nullptr;
    }

    bool init(ostd::Size size) {
        running = true;
        for (ostd::Size i = 0; i < size; ++i) {
            pthread_t tid;
            if (pthread_create(&tid, nullptr, thr_func, this))
                return false;
            thrs.push(tid);
        }
        return true;
    }

    void destroy() {
        mtx.lock();
        running = false;
        mtx.unlock();
        cond.broadcast();
        for (pthread_t &tid: thrs.iter()) {
            void *ret;
            pthread_join(tid, &ret);
            cond.broadcast();
        }
    }

    void run() {
        for (;;) {
            mtx.lock();
            while (running && (tasks == nullptr))
                cond.wait(mtx);
            if (!running) {
                mtx.unlock();
                pthread_exit(nullptr);
            }
            Task *t = tasks;
            tasks = t->next;
            if (last_task == t)
                last_task = nullptr;
            mtx.unlock();
            t->cb();
            delete t;
        }
    }

    void push(ostd::Function<void()> func) {
        mtx.lock();
        Task *t = new Task(ostd::move(func));
        if (last_task)
            last_task->next = t;
        last_task = t;
        if (!tasks)
            tasks = t;
        cond.signal();
        mtx.unlock();
    }

private:
    Cond cond;
    Mutex mtx;
    ostd::Vector<pthread_t> thrs;
    Task *tasks;
    Task *last_task;
    volatile bool running;
};

#endif /* OCTABUILD_TPOOL_HH */