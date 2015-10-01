#include <limits.h>
#include <unistd.h>

#include <ostd/types.hh>
#include <ostd/string.hh>
#include <ostd/vector.hh>
#include <ostd/atomic.hh>
#include <ostd/filesystem.hh>
#include <ostd/platform.hh>

#include <cubescript.hh>

#include "globs.hh"
#include "tpool.hh"

using ostd::ConstCharRange;
using ostd::Vector;
using ostd::String;
using ostd::Uint32;
using ostd::slice_until;

/* represents a rule definition, possibly with a function */
struct Rule {
    String target;
    Vector<String> deps;
    Uint32 *func;

    Rule(): target(), deps(), func(nullptr) {}
    Rule(const Rule &r): target(r.target), deps(r.deps), func(r.func) {
        cscript::bcode_ref(func);
    }
    ~Rule() { cscript::bcode_unref(func); }
};

static Vector<Rule> rules;

struct RuleCounter;
static Vector<RuleCounter *> counters;

struct RuleCounter {
    RuleCounter(): cond(), mtx(), counter(0), result(0) {
        counters.push(this);
    }

    void wait() {
        mtx.lock();
        while (counter)
            cond.wait(mtx);
        mtx.unlock();
    }

    void incr() {
        mtx.lock();
        ++counter;
        mtx.unlock();
    }

    void decr() {
        mtx.lock();
        if (!--counter) {
            mtx.unlock();
            cond.broadcast();
        } else
            mtx.unlock();
    }

    int wait_result(int ret) {
        counters.pop();
        if (ret)
            return ret;
        wait();
        ret = result;
        if (ret)
            return ret;
        return 0;
    }

    Cond cond;
    Mutex mtx;
    volatile int counter;
    ostd::AtomicInt result;
};

static ThreadPool tpool;

/* check funcs */

static bool ob_check_ts(ConstCharRange tname, const Vector<String> &deps) {
    auto get_ts = [](ConstCharRange fname) -> time_t {
        ostd::FileInfo fi(fname);
        if (fi.type() != ostd::FileType::regular)
            return 0;
        return fi.mtime();
    };
    time_t tts = get_ts(tname);
    if (!tts) return true;
    for (auto &dep: deps.iter()) {
        time_t sts = get_ts(dep);
        if (sts && (tts < sts)) return true;
    }
    return false;
}

static bool ob_check_file(ConstCharRange fname) {
    return ostd::FileStream(fname, ostd::StreamMode::read).is_open();
}

static bool ob_check_exec(ConstCharRange tname, const Vector<String> &deps) {
    if (!ob_check_file(tname))
        return true;
    for (auto &dep: deps.iter())
        if (!ob_check_file(dep))
            return true;
    return ob_check_ts(tname, deps);
}

/* this lets us properly match % patterns in target names */
static ConstCharRange ob_compare_subst(ConstCharRange expanded,
                                       ConstCharRange toexpand) {
    auto rep = ostd::find(toexpand, '%');
    /* no subst found */
    if (rep.empty())
        return nullptr;
    /* get the part before % */
    auto fp = slice_until(toexpand, rep);
    /* part before % does not compare, so ignore */
    if (expanded.size() <= fp.size())
        return nullptr;
    if (expanded.slice(0, fp.size()) != fp)
        return nullptr;
    /* pop out front part */
    expanded.pop_front_n(fp.size());
    /* part after % */
    rep.pop_front();
    if (rep.empty())
        return expanded;
    /* part after % does not compare, so ignore */
    if (expanded.size() <= rep.size())
        return nullptr;
    if (expanded.slice(expanded.size() - rep.size(), expanded.size()) != rep)
        return nullptr;
    /* cut off latter part */
    expanded.pop_back_n(rep.size());
    /* we got what we wanted... */
    return expanded;
}

struct ObState {
    cscript::CsState cs;
    ConstCharRange progname;
    int jobs = 1;

    struct SubRule {
        ConstCharRange sub;
        Rule *rule;
    };

    template<typename ...A>
    int error(int retcode, ConstCharRange fmt, A &&...args) {
        ostd::err.write(progname, ": ");
        ostd::err.writefln(fmt, ostd::forward<A>(args)...);
        return retcode;
    }

    int exec_list(const Vector<SubRule> &rlist, Vector<String> &subdeps,
                  ConstCharRange tname) {
        String repd;
        for (auto &sr: rlist.iter()) for (auto &target: sr.rule->deps.iter()) {
            ConstCharRange atgt = target.iter();
            repd.clear();
            auto lp = ostd::find(atgt, '%');
            if (!lp.empty()) {
                repd.append(slice_until(atgt, lp));
                repd.append(sr.sub);
                lp.pop_front();
                if (!lp.empty()) repd.append(lp);
                atgt = repd.iter();
            }
            subdeps.push(atgt);
            int r = exec_rule(atgt, tname);
            if (r) return r;
        }
        return 0;
    }

    int exec_func(ConstCharRange tname, const Vector<SubRule> &rlist) {
        Vector<String> subdeps;
        /* new scope for early destruction */
        {
            RuleCounter depcnt;
            int r = depcnt.wait_result(exec_list(rlist, subdeps, tname));
            if (r)
                return r;
        }
        if (ob_check_exec(tname, subdeps)) {
            Uint32 *func = nullptr;
            for (auto &sr: rlist.iter()) {
                Rule &r = *sr.rule;
                if (!r.func)
                    continue;
                if (func)
                    return error(1, "redefinition of rule '%s'", tname);
                func = r.func;
            }
            if (func) {
                cscript::StackedValue targetv, sourcev, sourcesv;

                targetv.id = cs.new_ident("target");
                if (!cscript::check_alias(targetv.id))
                    return 1;
                targetv.set_cstr(tname);
                targetv.push();

                if (subdeps.size() > 0) {
                    sourcev.id = cs.new_ident("source");
                    sourcesv.id = cs.new_ident("sources");
                    if (!cscript::check_alias(sourcev.id))
                        return 1;
                    if (!cscript::check_alias(sourcesv.id))
                        return 1;

                    sourcev.set_cstr(subdeps[0]);
                    sourcev.push();

                    auto dsv = ostd::appender<String>();
                    ostd::concat(dsv, subdeps);
                    ostd::Size len = dsv.size();
                    sourcesv.set_str(ostd::CharRange(dsv.get().disown(), len));
                    sourcesv.push();
                }

                return cs.run_int(func);
            }
        }
        return 0;
    }

    int exec_rule(ConstCharRange target, ConstCharRange from = nullptr) {
        Vector<SubRule> rlist;
        for (auto &rule: rules.iter()) {
            if (target == rule.target) {
                rlist.push().rule = &rule;
                continue;
            }
            ConstCharRange sub = ob_compare_subst(target, rule.target);
            if (!sub.empty()) {
                SubRule &sr = rlist.push();
                sr.rule = &rule;
                sr.sub = sub;
            }
        }
        if (rlist.empty() && !ob_check_file(target)) {
            if (from.empty())
                return error(1, "no rule to run target '%s'", target);
            else
                return error(1, "no rule to run target '%s' (needed by '%s')",
                             target, from);
            return 1;
        }
        return exec_func(target, rlist);
    }
};

static int ob_print_help(ConstCharRange a0, ostd::Stream &os, int v) {
    os.writeln("Usage: ", a0,  " [options] [target]\n",
               "Options:\n"
               "  -C DIRECTORY\tChange to DIRECTORY before running.\n",
               "  -f FILE\tSpecify the file to run (default: cubefile).\n"
               "  -h\t\tPrint this message.\n"
               "  -j N\t\tSpecify the number of jobs to use (default: 1).\n"
               "  -e STR\t\tEvaluate a string instead of a file.\n"
               "  -E\t\tIgnore environment variables.");
    return v;
}

static bool ignore_env = false;

int main(int argc, char **argv) {
    ObState os;
    ConstCharRange pn = argv[0];
    ConstCharRange lslash = ostd::find_last(pn, '/');
    if (!lslash.empty()) {
        lslash.pop_front();
        os.progname = lslash;
    } else
        os.progname = pn;

    cscript::init_lib_base(os.cs);
    cscript::init_lib_io(os.cs);
    cscript::init_lib_math(os.cs);
    cscript::init_lib_string(os.cs);
    cscript::init_lib_list(os.cs);

    int ncpus = ostd::cpu_count_get();
    os.cs.add_ident(cscript::ID_VAR, "numcpus", INT_MAX, 1, &ncpus);
    os.cs.add_ident(cscript::ID_VAR, "numjobs", INT_MAX, 1, &os.jobs);

    ConstCharRange fname = "cubefile";
    ConstCharRange fcont;

    int ac;
    while ((ac = getopt(argc, argv, "C:f:hj:e:E")) >= 0) {
        switch (ac) {
        case 'C':
            if (!ostd::directory_change(optarg))
                return os.error(1, "failed changing directory: %s", optarg);
            break;
        case 'f':
            fname = optarg;
            break;
        case 'e':
            fcont = optarg;
            break;
        case 'h':
            return ob_print_help(argv[0], ostd::out, 0);
        case 'j': {
            int val = atoi(optarg);
            if (!val) val = ncpus;
            os.jobs = ostd::max(1, val);
            break;
        }
        case 'E':
            ignore_env = true;
            break;
        default:
            return ob_print_help(argv[0], ostd::err, 1);
            break;
        }
    }

    tpool.init(os.jobs);

    os.cs.add_command("shell", "C", [](cscript::CsState &cs, ConstCharRange s) {
        RuleCounter *cnt = counters.back();
        cnt->incr();
        char *ds = String(s).disown();
        tpool.push([cnt, ds]() {
            int ret = system(ds);
            delete[] ds;
            if (ret && !cnt->result)
                cnt->result = ret;
            cnt->decr();
        });
        cs.result->set_int(0);
    });

    os.cs.add_command("rule", "sseN", [](cscript::CsState &, const char *tgt,
                                         const char *dep, ostd::Uint32 *body,
                                         int *numargs) {
        auto targets = cscript::util::list_explode(tgt);
        auto deps = cscript::util::list_explode(dep);
        for (auto &target: targets.iter()) {
            Rule &r = rules.push();
            r.target = target;
            if (*numargs > 2) {
                r.func = body;
                cscript::bcode_ref(body);
            }
            for (auto &dep: deps.iter())
                r.deps.push(dep);
        }
    });

    os.cs.add_command("getenv", "s", [](cscript::CsState &cs, const char *en) {
        if (ignore_env) {
            cs.result->set_cstr("");
            return;
        }
        const char *ret = getenv(en);
        if (!ret || !ret[0]) {
            cs.result->set_cstr("");
            return;
        }
        cs.result->set_str_dup(ret);
    });

    cs_register_globs(os.cs);

    if ((!fcont.empty() && !os.cs.run_bool(fcont)) || !os.cs.run_file(fname))
        return os.error(1, "failed creating rules");

    if (rules.empty())
        return os.error(1, "no targets");

    RuleCounter maincnt;
    int ret = os.exec_rule((optind < argc) ? argv[optind] : "all");
    ret = maincnt.wait_result(ret);
    if (ret)
        return ret;
    tpool.destroy();
    return 0;
}