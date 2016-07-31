#include <ostd/types.hh>
#include <ostd/functional.hh>
#include <ostd/string.hh>
#include <ostd/vector.hh>
#include <ostd/map.hh>
#include <ostd/atomic.hh>
#include <ostd/filesystem.hh>
#include <ostd/io.hh>
#include <ostd/platform.hh>
#include <ostd/utility.hh>
#include <ostd/environ.hh>
#include <ostd/thread.hh>
#include <ostd/mutex.hh>
#include <ostd/condition.hh>

#include <cubescript.hh>

void cs_register_globs(cscript::CsState &cs);

using ostd::ConstCharRange;
using ostd::Vector;
using ostd::Map;
using ostd::String;
using ostd::Uint32;
using ostd::slice_until;
using ostd::Thread;
using ostd::UniqueLock;
using ostd::Mutex;
using ostd::Condition;

using cscript::CsState;
using cscript::TvalRange;
using cscript::StackedValue;
using cscript::Bytecode;

/* thread pool */

struct ThreadPool {
    ThreadPool() {}

    ~ThreadPool() {
        if (running) {
            destroy();
        }
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

/* check funcs */

static bool ob_check_ts(ConstCharRange tname, Vector<String> const &deps) {
    auto get_ts = [](ConstCharRange fname) {
        ostd::FileInfo fi(fname);
        if (fi.type() != ostd::FileType::regular) {
            return time_t(0);
        }
        return fi.mtime();
    };
    time_t tts = get_ts(tname);
    if (!tts) {
        return true;
    }
    for (auto &dep: deps.iter()) {
        time_t sts = get_ts(dep);
        if (sts && (tts < sts)) {
            return true;
        }
    }
    return false;
}

static bool ob_check_file(ConstCharRange fname) {
    return ostd::FileStream(fname, ostd::StreamMode::read).is_open();
}

static bool ob_check_exec(ConstCharRange tname, Vector<String> const &deps) {
    if (!ob_check_file(tname)) {
        return true;
    }
    for (auto &dep: deps.iter()) {
        if (!ob_check_file(dep)) {
            return true;
        }
    }
    return ob_check_ts(tname, deps);
}

/* this lets us properly match % patterns in target names */
static ConstCharRange ob_compare_subst(
    ConstCharRange expanded, ConstCharRange toexpand
) {
    auto rep = ostd::find(toexpand, '%');
    /* no subst found */
    if (rep.empty()) {
        return nullptr;
    }
    /* get the part before % */
    auto fp = slice_until(toexpand, rep);
    /* part before % does not compare, so ignore */
    if (expanded.size() <= fp.size()) {
        return nullptr;
    }
    if (expanded.slice(0, fp.size()) != fp) {
        return nullptr;
    }
    /* pop out front part */
    expanded += fp.size();
    /* part after % */
    ++rep;
    if (rep.empty()) {
        return expanded;
    }
    /* part after % does not compare, so ignore */
    if (expanded.size() <= rep.size()) {
        return nullptr;
    }
    ostd::Size es = expanded.size();
    if (expanded.slice(es - rep.size(), es) != rep) {
        return nullptr;
    }
    /* cut off latter part */
    expanded.pop_back_n(rep.size());
    /* we got what we wanted... */
    return expanded;
}

static ThreadPool tpool;

static ConstCharRange deffile = "obuild.cfg";

struct ObState: CsState {
    ConstCharRange progname;
    int jobs = 1;
    bool ignore_env = false;

    /* represents a rule definition, possibly with a function */
    struct Rule {
        String target;
        Vector<String> deps;
        Bytecode func;
        bool action;

        Rule(): target(), deps(), func(), action(false) {}
        Rule(Rule const &r):
            target(r.target), deps(r.deps), func(r.func), action(r.action)
        {}
    };

    Vector<Rule> rules;

    struct SubRule {
        ConstCharRange sub;
        Rule *rule;
    };

    Map<ConstCharRange, Vector<SubRule>> cache;

    struct RuleCounter {
        RuleCounter(): cond(), mtx(), counter(0), result(0) {}

        void wait() {
            UniqueLock<Mutex> l(mtx);
            while (counter) {
                cond.wait(l);
            }
        }

        void incr() {
            UniqueLock<Mutex> l(mtx);
            ++counter;
        }

        void decr() {
            UniqueLock<Mutex> l(mtx);
            if (!--counter) {
                l.unlock();
                cond.broadcast();
            }
        }

        Condition cond;
        Mutex mtx;
        int volatile counter;
        ostd::AtomicInt result;
    };

    Vector<RuleCounter *> counters;

    template<typename F>
    int wait_result(F func) {
        RuleCounter ctr;
        counters.push(&ctr);
        int ret = func();
        counters.pop();
        if (ret) {
            return ret;
        }
        ctr.wait();
        return ctr.result;
    }

    template<typename ...A>
    int error(int retcode, ConstCharRange fmt, A &&...args) {
        ostd::err.write(progname, ": ");
        ostd::err.writefln(fmt, ostd::forward<A>(args)...);
        return retcode;
    }

    int exec_list(
        Vector<SubRule> const &rlist, Vector<String> &subdeps,
        ConstCharRange tname
    ) {
        String repd;
        for (auto &sr: rlist.iter()) for (auto &target: sr.rule->deps.iter()) {
            ConstCharRange atgt = target.iter();
            repd.clear();
            auto lp = ostd::find(atgt, '%');
            if (!lp.empty()) {
                repd.append(slice_until(atgt, lp));
                repd.append(sr.sub);
                ++lp;
                if (!lp.empty()) {
                    repd.append(lp);
                }
                atgt = repd.iter();
            }
            subdeps.push(atgt);
            int r = exec_rule(atgt, tname);
            if (r) {
                return r;
            }
        }
        return 0;
    }

    int exec_func(ConstCharRange tname, Vector<SubRule> const &rlist) {
        Vector<String> subdeps;
        int ret = wait_result([&rlist, &subdeps, &tname, this]() {
            return exec_list(rlist, subdeps, tname);
        });
        Bytecode *func = nullptr;
        bool act = false;
        for (auto &sr: rlist.iter()) {
            if (sr.rule->func) {
                func = &sr.rule->func;
                act = sr.rule->action;
                break;
            }
        }
        if ((!ret && (act || ob_check_exec(tname, subdeps))) && func) {
            StackedValue targetv, sourcev, sourcesv;
            CsState &cs = *this;

            if (!targetv.alias(cs, "target")) {
                return 1;
            }
            targetv.set_cstr(tname);
            targetv.push();

            if (!subdeps.empty()) {
                if (!sourcev.alias(cs, "source")) {
                    return 1;
                }
                if (!sourcesv.alias(cs, "sources")) {
                    return 1;
                }

                sourcev.set_cstr(subdeps[0]);
                sourcev.push();

                auto dsv = ostd::appender<String>();
                ostd::concat(dsv, subdeps);
                sourcesv.set_str(ostd::move(dsv.get()));
                sourcesv.push();
            }

            return run_int(*func);
        }
        return ret;
    }

    int exec_action(Rule *rule) {
        return run_int(rule->func);
    }

    int find_rules(ConstCharRange target, Vector<SubRule> &rlist) {
        if (!rlist.empty()) {
            return 0;
        }
        SubRule *frule = nullptr;
        bool exact = false;
        for (auto &rule: rules.iter()) {
            if (target == rule.target) {
                rlist.push().rule = &rule;
                if (rule.func) {
                    if (frule && exact) {
                        return error(1, "redefinition of rule '%s'", target);
                    }
                    if (!frule) {
                        frule = &rlist.back();
                    } else {
                        *frule = rlist.back();
                        rlist.pop();
                    }
                    exact = true;
                }
                continue;
            }
            if (exact || !rule.func) {
                continue;
            }
            ConstCharRange sub = ob_compare_subst(target, rule.target);
            if (!sub.empty()) {
                SubRule &sr = rlist.push();
                sr.rule = &rule;
                sr.sub = sub;
                if (frule) {
                    if (sub.size() == frule->sub.size()) {
                        return error(1, "redefinition of rule '%s'", target);
                    }
                    if (sub.size() < frule->sub.size()) {
                        *frule = sr;
                        rlist.pop();
                    }
                } else {
                    frule = &sr;
                }
            }
        }
        return 0;
    }

    int exec_rule(ConstCharRange target, ConstCharRange from = nullptr) {
        Vector<SubRule> &rlist = cache[target];
        int fret = find_rules(target, rlist);
        if (fret) {
            return fret;
        }
        if ((rlist.size() == 1) && rlist[0].rule->action) {
            return exec_action(rlist[0].rule);
        }
        if (rlist.empty() && !ob_check_file(target)) {
            if (from.empty()) {
                return error(1, "no rule to run target '%s'", target);
            } else {
                return error(
                    1, "no rule to run target '%s' (needed by '%s')",
                    target, from
                );
            }
            return 1;
        }
        return exec_func(target, rlist);
    }

    int exec_main(ConstCharRange target) {
        return wait_result([&target, this]() { return exec_rule(target); });
    }

    void rule_add(
        ConstCharRange tgt, ConstCharRange dep, Uint32 *body,
        bool action = false
    ) {
        auto targets = cscript::util::list_explode(tgt);
        for (auto &target: targets.iter()) {
            Rule &r = rules.push();
            r.target = target;
            r.action = action;
            r.func = body;
            r.deps = cscript::util::list_explode(dep);
        }
    }

    void rule_dup(
        ConstCharRange tgt, ConstCharRange ptgt, ConstCharRange dep,
        bool inherit_deps
    ) {
        Rule *oldr = nullptr;
        for (auto &rule: rules.iter()) {
            if (ptgt == rule.target) {
                oldr = &rule;
                break;
            }
        }
        if (!oldr) {
            return;
        }
        Rule &r = rules.push();
        r.target = tgt;
        r.action = oldr->action;
        r.func = oldr->func;
        r.deps = inherit_deps ? oldr->deps : cscript::util::list_explode(dep);
    }

    void register_rulecmds() {
        add_commandn("rule", "sseN", [](ObState &os, cscript::TvalRange args) {
            os.rule_add(
                args[0].get_strr(), args[1].get_strr(),
                (args[3].get_int() > 2) ? args[2].get_code() : nullptr
            );
        });

        add_commandn("action", "se", [](ObState &os, cscript::TvalRange args) {
            os.rule_add(args[0].get_strr(), nullptr, args[1].get_code(), true);
        });

        add_commandn("depend", "ss", [](ObState &os, cscript::TvalRange args) {
            os.rule_add(args[0].get_strr(), args[1].get_str().iter(), nullptr);
        });

        add_commandn("duprule", "sssN", [](ObState &os, cscript::TvalRange args) {
            os.rule_dup(
                args[0].get_strr(), args[1].get_strr(),
                args[2].get_strr(), args[3].get_int() <= 2
            );
        });
    }

    int print_help(bool error) {
        ostd::Stream &os = error ? ostd::err : ostd::out;
        os.writeln(
            "Usage: ", progname,  " [options] [action]\n",
            "Options:\n"
            "  -C DIRECTORY\tChange to DIRECTORY before running.\n",
            "  -f FILE\tSpecify the file to run (default: ", deffile, ").\n"
            "  -h\t\tPrint this message.\n"
            "  -j N\t\tSpecify the number of jobs to use (default: 1).\n"
            "  -e STR\tEvaluate a string instead of a file.\n"
            "  -E\t\tIgnore environment variables."
        );
        return error;
    }
};

int main(int argc, char **argv) {
    ObState osv;
    ConstCharRange pn = argv[0];
    ConstCharRange lslash = ostd::find_last(pn, '/');
    osv.progname = lslash.empty() ? pn : (lslash + 1);

    cscript::init_libs(osv);

    int ncpus = ostd::Thread::hardware_concurrency();
    osv.add_ident(cscript::ID_VAR, "numcpus", 4096, 1, &ncpus);
    osv.add_ident(cscript::ID_VAR, "numjobs", 4096, 1, &osv.jobs);

    ConstCharRange fcont;

    int posarg = argc;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            char argn = argv[i][1];
            if (argn == 'E') {
                osv.ignore_env = true;
                continue;
            } else if ((argn == 'h') || (!argv[i][2] && ((i + 1) >= argc))) {
                return osv.print_help(argn != 'h');
            }
            ConstCharRange val = (argv[i][2] == '\0') ? argv[++i] : &argv[i][2];
            switch (argn) {
            case 'C':
                if (!ostd::directory_change(val)) {
                    return osv.error(1, "failed changing directory: %s", val);
                }
                break;
            case 'f':
                deffile = val;
                break;
            case 'e':
                fcont = val;
                break;
            case 'j': {
                int ival = atoi(val.data());
                if (!ival) {
                    ival = ncpus;
                }
                osv.jobs = ostd::max(1, ival);
                break;
            }
            default:
                return osv.print_help(true);
            }
        } else {
            posarg = i;
            break;
        }
    }

    tpool.init(osv.jobs);

    osv.register_rulecmds();

    osv.add_command("shell", "C", [](ObState &os, ConstCharRange s) {
        auto cnt = os.counters.back();
        cnt->incr();
        tpool.push([cnt, ds = String(s)]() {
            int ret = system(ds.data());
            if (ret && !cnt->result) {
                cnt->result = ret;
            }
            cnt->decr();
        });
        os.result->set_int(0);
    });

    osv.add_commandn("getenv", "ss", [](ObState &os, TvalRange args) {
        if (os.ignore_env) {
            os.result->set_cstr("");
            return;
        }
        os.result->set_str(ostd::move(
            ostd::env_get(args[0].get_str()).value_or(args[1].get_str())
        ));
    });

    osv.add_command("extreplace", "sss", [](
        cscript::CsState &cs, char const *lst,
        char const *oldext, char const *newext
    ) {
        String ret;
        if (oldext[0] == '.') {
            ++oldext;
        }
        if (newext[0] == '.') {
            ++newext;
        }
        auto fnames = cscript::util::list_explode(lst);
        for (ConstCharRange it: fnames.iter()) {
            if (!ret.empty()) {
                ret += ' ';
            }
            auto dot = ostd::find_last(it, '.');
            if (!dot.empty() && ((dot + 1) == oldext)) {
                ret += ostd::slice_until(it, dot);
                ret += '.';
                ret += newext;
            } else {
                ret += it;
            }
        }
        cs.result->set_str(ostd::move(ret));
    });

    osv.add_command("invoke", "s", [](ObState &os, char const *name) {
        os.result->set_int(os.exec_main(name));
    });

    cs_register_globs(osv);

    if ((!fcont.empty() && !osv.run_bool(fcont)) || !osv.run_file(deffile)) {
        return osv.error(1, "failed creating rules");
    }

    if (osv.rules.empty()) {
        return osv.error(1, "no targets");
    }

    return osv.exec_main((posarg < argc) ? argv[posarg] : "default");
}
