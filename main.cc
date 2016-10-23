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
#include <ostd/mutex.hh>
#include <ostd/condition.hh>

#include <cubescript/cubescript.hh>

#include "tpool.hh"

using ostd::ConstCharRange;
using ostd::Vector;
using ostd::Map;
using ostd::String;
using ostd::slice_until;
using ostd::UniqueLock;
using ostd::Mutex;
using ostd::Condition;

using cscript::CsState;
using cscript::CsValueRange;
using cscript::CsValue;
using cscript::CsStackedValue;
using cscript::CsBytecodeRef;
using cscript::CsBytecode;

/* glob matching code */

static void ob_get_path_parts(
    Vector<ConstCharRange> &parts, ConstCharRange elem
) {
    ConstCharRange star = ostd::find(elem, '*');
    while (!star.empty()) {
        ConstCharRange ep = slice_until(elem, star);
        if (!ep.empty()) {
            parts.push(ep);
        }
        parts.push("*");
        elem = star;
        ++elem;
        star = ostd::find(elem, '*');
    }
    if (!elem.empty()) {
        parts.push(elem);
    }
}

static bool ob_path_matches(
    ConstCharRange fn, Vector<ConstCharRange> const &parts
) {
    for (auto it = parts.iter(); !it.empty(); ++it) {
        ConstCharRange elem = it.front();
        if (elem == "*") {
            ++it;
            /* skip multiple stars if present */
            while (!it.empty() && ((elem = it.front()) == "*")) {
                ++it;
            }
            /* trailing stars, we match */
            if (it.empty()) {
                return true;
            }
            /* skip about as much as we can until the elem part matches */
            while (fn.size() > elem.size()) {
                if (fn.slice(0, elem.size()) == elem) {
                    break;
                }
                ++fn;
            }
        }
        /* non-star here */
        if (fn.size() < elem.size()) {
            return false;
        }
        if (fn.slice(0, elem.size()) != elem) {
            return false;
        }
        fn += elem.size();
    }
    /* if there are no chars in the fname remaining, we fully matched */
    return fn.empty();
}

static bool ob_expand_glob(String &ret, ConstCharRange src, bool ne = false);

static bool ob_expand_dir(
    String &ret, ConstCharRange dir, Vector<ConstCharRange> const &parts,
    ConstCharRange slash
) {
    ostd::DirectoryStream d(dir);
    bool appended = false;
    if (!d.is_open()) {
        return false;
    }
    for (auto fi: d.iter()) {
        ConstCharRange fn = fi.filename();
        /* check if filename matches */
        if (!ob_path_matches(fn, parts)) {
            continue;
        }
        String afn((dir == ".") ? "" : "./");
        afn.append(fn);
        /* if we reach this, we match; try recursively matching */
        if (!slash.empty()) {
            afn.append(slash);
            ConstCharRange psl = slash + 1;
            if (!ostd::find(psl, '*').empty()) {
                if (!appended) {
                    appended = ob_expand_glob(ret, afn.iter());
                }
                continue;
            }
            /* no further star, just do file test */
            if (!ostd::FileStream(afn, ostd::StreamMode::read).is_open()) {
                continue;
            }
            if (!ret.empty()) {
                ret.push(' ');
            }
            ret.append(afn);
            appended = true;
            continue;
        }
        if (!ret.empty()) {
            ret.push(' ');
        }
        ret.append(afn);
        appended = true;
    }
    return appended;
}

static bool ob_expand_glob(String &ret, ConstCharRange src, bool ne) {
    ConstCharRange star = ostd::find(src, '*');
    /* no star use as-is */
    if (star.empty()) {
        if (ne) return false;
        if (!ret.empty()) {
            ret.push(' ');
        }
        ret.append(src);
        return false;
    }
    /* part before star */
    ConstCharRange prestar = slice_until(src, star);
    /* try finding slash before star */
    ConstCharRange slash = ostd::find_last(prestar, '/');
    /* directory to scan */
    ConstCharRange dir = ".";
    /* part of name before star */
    ConstCharRange fnpre = prestar;
    if (!slash.empty()) {
        /* there was slash, adjust directory + prefix accordingly */
        dir = slice_until(src, slash);
        fnpre = slash + 1;
    }
    /* part after star */
    ConstCharRange fnpost = star + 1;
    /* if a slash follows, adjust */
    ConstCharRange nslash = ostd::find(fnpost, '/');
    if (!nslash.empty()) {
        fnpost = slice_until(fnpost, nslash);
    }
    /* retrieve the single element with whatever stars in it, chop it up */
    Vector<ConstCharRange> parts;
    ob_get_path_parts(parts, ConstCharRange(&fnpre[0], &fnpost[fnpost.size()]));
    /* do a directory scan and match */
    if (!ob_expand_dir(ret, dir, parts, nslash)) {
        if (ne) {
            return false;
        }
        if (!ret.empty()) {
            ret.push(' ');
        }
        ret.append(src);
        return false;
    }
    return true;
}

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

struct ObState: CsState {
    ConstCharRange progname;
    bool ignore_env = false;

    /* represents a rule definition, possibly with a function */
    struct Rule {
        String target;
        Vector<String> deps;
        CsBytecodeRef func;
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
        RuleCounter(): p_cond(), p_mtx(), p_counter(0), p_result(0) {}

        void wait() {
            UniqueLock<Mutex> l(p_mtx);
            while (p_counter) {
                p_cond.wait(l);
            }
        }

        void incr() {
            UniqueLock<Mutex> l(p_mtx);
            ++p_counter;
        }

        void decr() {
            UniqueLock<Mutex> l(p_mtx);
            if (!--p_counter) {
                l.unlock();
                p_cond.broadcast();
            }
        }

        ostd::AtomicInt &get_result() { return p_result; }

    private:
        Condition p_cond;
        Mutex p_mtx;
        int p_counter;
        ostd::AtomicInt p_result;
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
        return ctr.get_result();
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
        for (auto &sr: rlist.iter()) {
            for (auto &target: sr.rule->deps.iter()) {
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
        }
        return 0;
    }

    int exec_func(ConstCharRange tname, Vector<SubRule> const &rlist) {
        Vector<String> subdeps;
        int ret = wait_result([&rlist, &subdeps, &tname, this]() {
            return exec_list(rlist, subdeps, tname);
        });
        CsBytecodeRef *func = nullptr;
        bool act = false;
        for (auto &sr: rlist.iter()) {
            if (sr.rule->func) {
                func = &sr.rule->func;
                act = sr.rule->action;
                break;
            }
        }
        if ((!ret && (act || ob_check_exec(tname, subdeps))) && func) {
            CsStackedValue targetv, sourcev, sourcesv;

            if (!targetv.set_alias(new_ident("target"))) {
                return 1;
            }
            targetv.set_cstr(tname);
            targetv.push();

            if (!subdeps.empty()) {
                if (!sourcev.set_alias(new_ident("source"))) {
                    return 1;
                }
                if (!sourcesv.set_alias(new_ident("sources"))) {
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
        CsState &cs, ConstCharRange tgt, ConstCharRange dep, CsBytecode *body,
        bool action = false
    ) {
        cscript::util::ListParser p{cs, tgt};
        while (p.parse()) {
            Rule &r = rules.push();
            r.target = p.element();
            r.action = action;
            r.func = cscript::cs_code_is_empty(body) ? nullptr : body;
            cscript::util::ListParser lp{cs, dep};
            while (lp.parse()) {
                r.deps.push(lp.element());
            }
        }
    }

    void rule_dup(
        CsState &cs, ConstCharRange tgt, ConstCharRange ptgt,
        ConstCharRange dep, bool inherit_deps
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
        if (inherit_deps) {
            r.deps = oldr->deps;
        } else {
            cscript::util::ListParser p{cs, dep};
            while (p.parse()) {
                r.deps.push(p.element());
            }
        }
    }

    void register_rulecmds() {
        new_command("rule", "sse", [this](auto &cs, auto args, auto &) {
            rule_add(
                cs, args[0].get_strr(), args[1].get_strr(), args[2].get_code()
            );
        });

        new_command("action", "se", [this](auto &cs, auto args, auto &) {
            rule_add(cs, args[0].get_strr(), nullptr, args[1].get_code(), true);
        });

        new_command("depend", "ss", [this](auto &cs, auto args, auto &) {
            rule_add(cs, args[0].get_strr(), args[1].get_strr(), nullptr);
        });

        new_command("duprule", "sssN", [this](auto &cs, auto args, auto &) {
            rule_dup(
                cs, args[0].get_strr(), args[1].get_strr(),
                args[2].get_strr(), args[3].get_int() <= 2
            );
        });
    }

    int print_help(bool error, ConstCharRange deffile) {
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
    ObState os;
    ConstCharRange pn = argv[0];
    ConstCharRange lslash = ostd::find_last(pn, '/');
    os.progname = lslash.empty() ? pn : (lslash + 1);

    os.init_libs();

    int ncpus = ostd::Thread::hardware_concurrency();
    os.new_ivar("numcpus", 4096, 1, ncpus);

    ConstCharRange fcont;
    ConstCharRange deffile = "obuild.cfg";

    int jobs = 1, posarg = argc;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            char argn = argv[i][1];
            if (argn == 'E') {
                os.ignore_env = true;
                continue;
            } else if ((argn == 'h') || (!argv[i][2] && ((i + 1) >= argc))) {
                return os.print_help(argn != 'h', deffile);
            }
            ConstCharRange val = (argv[i][2] == '\0') ? argv[++i] : &argv[i][2];
            switch (argn) {
                case 'C':
                    if (!ostd::directory_change(val)) {
                        return os.error(1, "failed changing directory: %s", val);
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
                    jobs = ostd::max(1, ival);
                    break;
                }
                default:
                    return os.print_help(true, deffile);
            }
        } else {
            posarg = i;
            break;
        }
    }
    os.new_ivar("numjobs", 4096, 1, jobs);

    ThreadPool tpool;
    tpool.init(jobs);

    os.register_rulecmds();

    os.new_command("echo", "C", [](auto &, auto args, auto &) {
        writeln(args[0].get_strr());
    });

    os.new_command("shell", "C", [&os, &tpool](auto &, auto args, auto &res) {
        auto cnt = os.counters.back();
        cnt->incr();
        tpool.push([cnt, ds = String(args[0].get_strr())]() {
            int ret = system(ds.data());
            if (ret && !cnt->get_result()) {
                cnt->get_result() = ret;
            }
            cnt->decr();
        });
        res.set_int(0);
    });

    os.new_command("getenv", "ss", [&os](auto &, auto args, auto &res) {
        if (os.ignore_env) {
            res.set_cstr("");
            return;
        }
        res.set_str(ostd::move(
            ostd::env_get(args[0].get_str()).value_or(args[1].get_str())
        ));
    });

    os.new_command("extreplace", "sss", [](auto &cs, auto args, auto &res) {
        ConstCharRange lst = args[0].get_strr();
        ConstCharRange oldext = args[1].get_strr();
        ConstCharRange newext = args[2].get_strr();
        String ret;
        if (oldext.front() == '.') {
            oldext.pop_front();
        }
        if (newext.front() == '.') {
            newext.pop_front();
        }
        cscript::util::ListParser p{cs, lst};
        while (p.parse()) {
            auto elem = p.element();
            ConstCharRange it = elem.iter();
            if (!ret.empty()) {
                ret += ' ';
            }
            auto dot = ostd::find_last(it, '.');
            if (!dot.empty() && ((dot + 1) == oldext)) {
                ret += slice_until(it, dot);
                ret += '.';
                ret += newext;
            } else {
                ret += it;
            }
        }
        res.set_str(ostd::move(ret));
    });

    os.new_command("invoke", "s", [&os](auto &, auto args, auto &res) {
        res.set_int(os.exec_main(args[0].get_strr()));
    });

    os.new_command("glob", "C", [&os](auto &cs, auto args, auto &res) {
        String ret;
        cscript::util::ListParser p{cs, args[0].get_strr()};
        while (p.parse()) {
            ob_expand_glob(ret, p.element());
        }
        res.set_str(ostd::move(ret));
    });

    if ((!fcont.empty() && !os.run_bool(fcont)) || !os.run_file(deffile)) {
        return os.error(1, "failed creating rules");
    }

    if (os.rules.empty()) {
        return os.error(1, "no targets");
    }

    return os.exec_main((posarg < argc) ? argv[posarg] : "default");
}
