#include <sys/stat.h>

#include <ostd/types.hh>
#include <ostd/string.hh>
#include <ostd/vector.hh>

#include <cubescript.hh>

struct Rule {
    ostd::String target;
    ostd::Vector<ostd::String> deps;
    ostd::Uint32 *func;

    Rule(): target(), deps(), func(nullptr) {}
    Rule(const Rule &r): target(r.target), deps(r.deps), func(r.func) {
        cscript::bcode_ref(func);
    }
    ~Rule() { cscript::bcode_unref(func); }
};

struct ObState {
    cscript::CsState cs;
    ostd::ConstCharRange progname;
};

ostd::Vector<Rule> rules;

static time_t ob_get_file_ts(const char *fname) {
    struct stat st;
    if (stat(fname, &st) < 0) return 0;
    return st.st_mtime;
}

static bool ob_check_ts(ostd::ConstCharRange tname,
                        const ostd::Vector<ostd::String> &deps) {
    time_t tts = ob_get_file_ts(tname.data());
    if (!tts) return true;
    for (auto &dep: deps.iter()) {
        time_t sts = ob_get_file_ts(dep.data());
        if (sts && tts < sts) return true;
    }
    return false;
}

static bool ob_check_file(ostd::ConstCharRange fname) {
    return ostd::FileStream(fname, ostd::StreamMode::read).is_open();
}

static bool ob_check_exec(ostd::ConstCharRange tname,
                          const ostd::Vector<ostd::String> &deps) {
    if (!ob_check_file(tname))
        return true;
    for (auto &dep: deps.iter())
        if (!ob_check_file(dep))
            return true;
    return ob_check_ts(tname, deps);
}

template<typename ...A>
static int ob_error(ObState &os, int retcode, ostd::ConstCharRange fmt,
                    A &&...args) {
    ostd::err.write(os.progname, ": ");
    ostd::err.writefln(fmt, ostd::forward<A>(args)...);
    return retcode;
}

static int ob_exec_rule(ObState &os, ostd::ConstCharRange target,
                        ostd::ConstCharRange from = ostd::ConstCharRange());

struct SubRule {
    ostd::ConstCharRange sub;
    Rule *rule;
};

static int ob_exec_rlist(ObState &os,
                         const ostd::Vector<SubRule> &rlist,
                         ostd::Vector<ostd::String> &subdeps,
                         ostd::ConstCharRange tname) {
    ostd::String repd;
    for (auto &sr: rlist.iter()) for (auto &target: sr.rule->deps.iter()) {
        ostd::ConstCharRange atgt = target.iter();
        repd.clear();
        auto lp = ostd::find(atgt, '%');
        if (!lp.empty()) {
            repd.append(ostd::slice_until(atgt, lp));
            repd.append(sr.sub);
            lp.pop_front();
            if (!lp.empty()) repd.append(lp);
            atgt = repd.iter();
        }
        subdeps.push(atgt);
        int r = ob_exec_rule(os, atgt, tname);
        if (r) return r;
    }
    return 0;
}

static int ob_exec_func(ObState &os, ostd::ConstCharRange tname,
                        const ostd::Vector<SubRule> &rlist) {
    ostd::Vector<ostd::String> subdeps;
    int r = ob_exec_rlist(os, rlist, subdeps, tname);
    if (ob_check_exec(tname, subdeps)) {
        if (r) return r;
        ostd::Uint32 *func = nullptr;
        for (auto &sr: rlist.iter()) {
            Rule &r = *sr.rule;
            if (!r.func)
                continue;
            if (func)
                return ob_error(os, 1, "redefinition of rule '%s'", tname);
            func = r.func;
        }
        if (func) {
            cscript::StackedValue targetv, sourcev, sourcesv;

            targetv.id = os.cs.new_ident("target");
            if (!cscript::check_alias(targetv.id))
                return 1;
            targetv.set_cstr(tname);
            targetv.push();

            if (subdeps.size() > 0) {
                sourcev.id = os.cs.new_ident("source");
                sourcesv.id = os.cs.new_ident("sources");
                if (!cscript::check_alias(sourcev.id))
                    return 1;
                if (!cscript::check_alias(sourcesv.id))
                    return 1;

                sourcev.set_cstr(subdeps[0]);
                sourcev.push();

                auto dsv = ostd::appender<ostd::String>();
                ostd::concat(dsv, subdeps);
                ostd::Size len = dsv.size();
                sourcesv.set_str(ostd::CharRange(dsv.get().disown(), len));
                sourcesv.push();
            }

            return os.cs.run_int(func);
        }
    }
    return 0;
}

static ostd::ConstCharRange ob_compare_subst(ostd::ConstCharRange expanded,
                                             ostd::ConstCharRange toexpand) {
    auto rep = ostd::find(toexpand, '%');
    /* no subst found */
    if (rep.empty())
        return ostd::ConstCharRange();
    /* get the part before % */
    auto fp = ostd::slice_until(toexpand, rep);
    /* part before % does not compare, so ignore */
    if (expanded.size() <= fp.size())
        return ostd::ConstCharRange();
    if (expanded.slice(0, fp.size()) != fp)
        return ostd::ConstCharRange();
    /* pop out front part */
    expanded.pop_front_n(fp.size());
    /* part after % */
    rep.pop_front();
    if (rep.empty())
        return expanded;
    /* part after % does not compare, so ignore */
    if (expanded.size() <= rep.size())
        return ostd::ConstCharRange();
    if (expanded.slice(expanded.size() - rep.size(), expanded.size()) != rep)
        return ostd::ConstCharRange();
    /* cut off latter part */
    expanded.pop_back_n(rep.size());
    /* we got what we wanted... */
    return expanded;
}

static int ob_exec_rule(ObState &os, ostd::ConstCharRange target,
                        ostd::ConstCharRange from) {
    ostd::Vector<SubRule> rlist;
    for (auto &rule: rules.iter()) {
        if (target == rule.target) {
            rlist.push().rule = &rule;
            continue;
        }
        ostd::ConstCharRange sub = ob_compare_subst(target, rule.target);
        if (!sub.empty()) {
            SubRule &sr = rlist.push();
            sr.rule = &rule;
            sr.sub = sub;
        }
    }
    if (rlist.empty() && !ob_check_file(target)) {
        if (from.empty())
            return ob_error(os, 1, "no rule to run target '%s'", target);
        else
            return ob_error(os, 1, "no rule to run target '%s' "
                                   "(needed by '%s')",
                            target, from);
        return 1;
    }
    return ob_exec_func(os, target, rlist);
}

static void ob_rule_cmd(cscript::CsState &, const char *tgt, const char *dep,
                        ostd::Uint32 *body, int *numargs) {
    ostd::Vector<ostd::String> targets = cscript::util::list_explode(tgt);
    ostd::Vector<ostd::String> deps = cscript::util::list_explode(dep);
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
}

int main(int argc, char **argv) {
    ObState os;
    ostd::ConstCharRange pn = argv[0];
    ostd::ConstCharRange lslash = ostd::find_last(pn, '/');
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

    os.cs.add_command("shell", "C", [](cscript::CsState &cs, char *s) {
        cs.result->set_int(system(s));
    });

    os.cs.add_command("rule", "sseN", ob_rule_cmd);

    if (!os.cs.run_file("cubefile", true))
        return ob_error(os, 1, "failed creating rules");

    if (rules.empty())
        return ob_error(os, 1, "no targets");

    return ob_exec_rule(os, (argc > 1) ? argv[1] : "all");
}