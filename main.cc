#include <unistd.h>

#include <ostd/types.hh>
#include <ostd/string.hh>
#include <ostd/vector.hh>
#include <ostd/map.hh>
#include <ostd/atomic.hh>
#include <ostd/filesystem.hh>
#include <ostd/platform.hh>

#include <cubescript.hh>

#include "globs.hh"
#include "tpool.hh"

using ostd::ConstCharRange;
using ostd::Vector;
using ostd::Map;
using ostd::String;
using ostd::Uint32;
using ostd::slice_until;

using cscript::CsState;
using cscript::TvalRange;
using cscript::StackedValue;

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

static ConstCharRange ob_get_env(ConstCharRange ename) {
    return getenv(ostd::String(ename).data());
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
    ostd::Size es = expanded.size();
    if (expanded.slice(es - rep.size(), es) != rep)
        return nullptr;
    /* cut off latter part */
    expanded.pop_back_n(rep.size());
    /* we got what we wanted... */
    return expanded;
}

static ThreadPool tpool;

struct ObState {
    CsState cs;
    ConstCharRange progname;
    int jobs = 1;
    bool ignore_env = false;

    /* represents a rule definition, possibly with a function */
    struct Rule {
        String target;
        Vector<String> deps;
        Uint32 *func;
        bool action;

        Rule(): target(), deps(), func(nullptr), action(false) {}
        Rule(const Rule &r): target(r.target), deps(r.deps), func(r.func),
                             action(r.action) {
            cscript::bcode_ref(func);
        }
        ~Rule() { cscript::bcode_unref(func); }
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

        Cond cond;
        Mutex mtx;
        volatile int counter;
        ostd::AtomicInt result;
    };

    Vector<RuleCounter *> counters;

    template<typename F>
    int wait_result(F func) {
        RuleCounter ctr;
        counters.push(&ctr);
        int ret = func();
        counters.pop();
        if (ret) return ret;
        ctr.wait();
        return ctr.result;
    }

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
        int ret = wait_result([&rlist, &subdeps, &tname, this]() {
            return exec_list(rlist, subdeps, tname);
        });
        Uint32 *func = nullptr;
        bool act = false;
        for (auto &sr: rlist.iter()) {
            Rule &r = *sr.rule;
            if (r.func) {
                func = r.func;
                act = r.action;
                break;
            }
        }
        if ((!ret && (act || ob_check_exec(tname, subdeps))) && func) {
            StackedValue targetv, sourcev, sourcesv;

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
                sourcesv.set_str(ostd::CharRange(dsv.get().disown(),
                                                 len));
                sourcesv.push();
            }

            return cs.run_int(func);
        }
        return ret;
    }

    int exec_action(Rule *rule) {
        return cs.run_int(rule->func);
    }

    int find_rules(ConstCharRange target, Vector<SubRule> &rlist) {
        if (!rlist.empty())
            return 0;
        SubRule *frule = nullptr;
        bool exact = false;
        for (auto &rule: rules.iter()) {
            if (target == rule.target) {
                rlist.push().rule = &rule;
                if (rule.func) {
                    if (frule && exact)
                        return error(1, "redefinition of rule '%s'",
                                     target);
                    if (!frule)
                        frule = &rlist.back();
                    else {
                        *frule = rlist.back();
                        rlist.pop();
                    }
                    exact = true;
                }
                continue;
            }
            if (exact || !rule.func)
                continue;
            ConstCharRange sub = ob_compare_subst(target, rule.target);
            if (!sub.empty()) {
                SubRule &sr = rlist.push();
                sr.rule = &rule;
                sr.sub = sub;
                if (frule) {
                    if (sub.size() == frule->sub.size())
                        return error(1, "redefinition of rule '%s'",
                                     target);
                    if (sub.size() < frule->sub.size()) {
                        *frule = sr;
                        rlist.pop();
                    }
                } else frule = &sr;
            }
        }
        return 0;
    }

    int exec_rule(ConstCharRange target, ConstCharRange from = nullptr) {
        Vector<SubRule> &rlist = cache[target];
        int fret = find_rules(target, rlist);
        if (fret)
            return fret;
        if ((rlist.size() == 1) && rlist[0].rule->action)
            return exec_action(rlist[0].rule);
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

    int exec_main(ConstCharRange target) {
        return wait_result([&target, this]() { return exec_rule(target); });
    }

    void rule_add(ConstCharRange tgt, ConstCharRange dep, ostd::Uint32 *body,
                  bool action = false) {
        auto targets = cscript::util::list_explode(tgt);
        for (auto &target: targets.iter()) {
            Rule &r = rules.push();
            r.target = target;
            r.action = action;
            if (body) {
                r.func = body;
                cscript::bcode_ref(body);
            }
            r.deps = dep ? cscript::util::list_explode(dep)
                         : ostd::Vector<ostd::String>();
        }
    }

    void rule_dup(const char *tgt, const char *ptgt, const char *dep) {
        Rule *oldr = nullptr;
        for (auto &rule: rules.iter())
            if (ptgt == rule.target)
                oldr = &rule;
        if (!oldr)
            return;
        Rule &r = rules.push();
        r.target = tgt;
        r.action = oldr->action;
        r.func = oldr->func;
        r.deps = dep ? cscript::util::list_explode(dep) : oldr->deps;
    }
};

static ConstCharRange deffile = "obuild.cfg";

static int ob_print_help(ConstCharRange a0, ostd::Stream &os, int v) {
    os.writeln("Usage: ", a0,  " [options] [action]\n",
               "Options:\n"
               "  -C DIRECTORY\tChange to DIRECTORY before running.\n",
               "  -f FILE\tSpecify the file to run (default: ", deffile, ").\n"
               "  -h\t\tPrint this message.\n"
               "  -j N\t\tSpecify the number of jobs to use (default: 1).\n"
               "  -e STR\tEvaluate a string instead of a file.\n"
               "  -E\t\tIgnore environment variables.");
    return v;
}

int main(int argc, char **argv) {
    ObState os;
    ConstCharRange pn = argv[0];
    ConstCharRange lslash = ostd::find_last(pn, '/');
    if (!lslash.empty()) {
        lslash.pop_front();
        os.progname = lslash;
    } else {
        os.progname = pn;
    }

    cscript::init_lib_base(os.cs);
    cscript::init_lib_io(os.cs);
    cscript::init_lib_math(os.cs);
    cscript::init_lib_string(os.cs);
    cscript::init_lib_list(os.cs);

    int ncpus = ostd::cpu_count_get();
    os.cs.add_ident(cscript::ID_VAR, "numcpus", 4096, 1, &ncpus);
    os.cs.add_ident(cscript::ID_VAR, "numjobs", 4096, 1, &os.jobs);

    ConstCharRange fcont;

    int ac;
    while ((ac = getopt(argc, argv, "C:f:hj:e:E")) >= 0) {
        switch (ac) {
        case 'C':
            if (!ostd::directory_change(optarg))
                return os.error(1, "failed changing directory: %s", optarg);
            break;
        case 'f':
            deffile = optarg;
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
            os.ignore_env = true;
            break;
        default:
            return ob_print_help(argv[0], ostd::err, 1);
        }
    }

    tpool.init(os.jobs);

    os.cs.add_command("shell", "C", [](CsState &cs, ConstCharRange s) {
        auto cnt = ((ObState &)cs).counters.back();
        cnt->incr();
        String ds = s;
        /* in c++14 we can use generalized lambda captures to move the str */
        tpool.push([cnt, ds]() {
            int ret = system(ds.data());
            if (ret && !cnt->result)
                cnt->result = ret;
            cnt->decr();
        });
        cs.result->set_int(0);
    });

    os.cs.add_command("rule", "sseN", [](CsState &cs, const char *tgt,
                                         const char *dep, ostd::Uint32 *body,
                                         int *numargs) {
        ((ObState &)cs).rule_add(tgt, dep, (*numargs > 2) ? body : nullptr);
    });

    os.cs.add_command("action", "se", [](CsState &cs, const char *an,
                                         ostd::Uint32 *body) {
        ((ObState &)cs).rule_add(an, nullptr, body, true);
    });

    os.cs.add_command("depend", "ss", [](CsState &cs, const char *file,
                                         const char *deps) {
        ((ObState &)cs).rule_add(file, deps, nullptr);
    });

    os.cs.add_command("duprule", "sssN", [](CsState &cs, const char *tgt,
                                            const char *ptgt, const char *dep,
                                            int *numargs) {
        ((ObState &)cs).rule_dup(tgt, ptgt, (*numargs > 2) ? dep : nullptr);
    });

    os.cs.add_commandn("getenv", "ss", [](CsState &cs, TvalRange args) {
        if (((ObState &)cs).ignore_env) {
            cs.result->set_cstr("");
            return;
        }
        auto ret = ob_get_env(args[0].get_str());
        if (ret.empty()) {
            if (!args[1].get_str().empty())
                cs.result->set_str_dup(args[1].get_str());
            else
                cs.result->set_cstr("");
        } else {
            cs.result->set_str_dup(ret);
        }
    });

    os.cs.add_command("extreplace", "sss", [](cscript::CsState &cs,
                                              const char *lst,
                                              const char *oldext,
                                              const char *newext) {
        String ret;
        if (oldext[0] == '.') ++oldext;
        if (newext[0] == '.') ++newext;
        auto fnames = cscript::util::list_explode(lst);
        for (ConstCharRange it: fnames.iter()) {
            if (!ret.empty()) ret += ' ';
            auto dot = ostd::find_last(it, '.');
            if (!dot.empty() && ((dot + 1) == oldext)) {
                ret += ostd::slice_until(it, dot);
                ret += '.';
                ret += newext;
            } else {
                ret += it;
            }
        }
        cs.result->set_str_dup(ret);
    });

    os.cs.add_command("invoke", "s", [](CsState &cs, const char *name) {
        cs.result->set_int(((ObState &)cs).exec_main(name));
    });

    cs_register_globs(os.cs);

    if ((!fcont.empty() && !os.cs.run_bool(fcont)) || !os.cs.run_file(deffile))
        return os.error(1, "failed creating rules");

    if (os.rules.empty())
        return os.error(1, "no targets");

    return os.exec_main((optind < argc) ? argv[optind] : "default");
}