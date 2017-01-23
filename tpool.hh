#ifndef OBUILD_TPOOL_HH
#define OBUILD_TPOOL_HH

#include <utility>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>

struct thread_pool {
    thread_pool():
        cond(), mtx(), thrs(), tasks(nullptr), last_task(nullptr),
        running(false)
    {}

    ~thread_pool() {
        destroy();
    }

    static void *thr_func(void *ptr) {
        static_cast<thread_pool *>(ptr)->run();
        return nullptr;
    }

    bool init(size_t size) {
        running = true;
        for (size_t i = 0; i < size; ++i) {
            std::thread tid{[this]() { run(); }};
            if (!tid.joinable()) {
                return false;
            }
            thrs.push_back(std::move(tid));
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
        cond.notify_all();
        for (std::thread &tid: thrs) {
            tid.join();
            cond.notify_all();
        }
    }

    void run() {
        for (;;) {
            std::unique_lock<std::mutex> l(mtx);
            while (running && (tasks == nullptr)) {
                cond.wait(l);
            }
            if (!running) {
                return;
            }
            task *t = tasks;
            tasks = t->next;
            if (last_task == t) {
                last_task = nullptr;
            }
            l.unlock();
            t->cb();
            delete t;
        }
    }

    void push(std::function<void()> func) {
        mtx.lock();
        task *t = new task(std::move(func));
        if (last_task) {
            last_task->next = t;
        }
        last_task = t;
        if (!tasks) {
            tasks = t;
        }
        cond.notify_one();
        mtx.unlock();
    }

private:
    struct task {
        std::function<void()> cb;
        task *next = nullptr;
        task() = delete;
        task(task const &) = delete;
        task(task &&) = delete;
        task(std::function<void()> &&cbf): cb(ostd::move(cbf)) {}
        task &operator=(task const &) = delete;
        task &operator=(task &&) = delete;
    };

    std::condition_variable cond;
    std::mutex mtx;
    std::vector<std::thread> thrs;
    task *tasks;
    task *last_task;
    bool volatile running;
};

#endif