#include <sys/stat.h>

#include <ostd/vector.hh>

#include "cubescript.hh"

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

ostd::Vector<Rule> rules;

time_t get_file_ts(const char *fname) {
    struct stat st;
    if (stat(fname, &st) < 0) return 0;
    return st.st_mtime;
}

bool check_ts(ostd::ConstCharRange tname,
              const ostd::Vector<ostd::String> &deps) {
    time_t tts = get_file_ts(tname.data());
    if (!tts) return true;
    for (auto &dep: deps.iter()) {
        time_t sts = get_file_ts(dep.data());
        if (sts && tts < sts) return true;
    }
    return false;
}

bool check_exec(ostd::ConstCharRange tname,
                const ostd::Vector<ostd::String> &deps) {
    ostd::FileStream f(tname, ostd::StreamMode::read);
    if (!f.is_open()) return true;
    f.close();
    for (auto &dep: deps.iter()) {
        f.open(dep, ostd::StreamMode::read);
        if (!f.is_open()) return true;
        f.close();
    }
    return check_ts(tname, deps);
}

int exec(cscript::CsState &cs, ostd::ConstCharRange target);

struct SubRule {
    ostd::ConstCharRange sub;
    Rule *rule;
};

int exec_list(cscript::CsState &cs,
              const ostd::Vector<SubRule> &rlist,
              ostd::Vector<ostd::String> &subdeps) {
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
        int r = exec(cs, atgt);
        if (r) return r;
    }
    return 0;
}

int exec_func(cscript::CsState &cs, ostd::ConstCharRange tname,
              const ostd::Vector<SubRule> &rlist) {
    ostd::Vector<ostd::String> subdeps;
    int r = exec_list(cs, rlist, subdeps);
    if (check_exec(tname, subdeps)) {
        if (r) return r;
        ostd::Uint32 *func = nullptr;
        for (auto &sr: rlist.iter()) {
            Rule &r = *sr.rule;
            if (!r.func)
                continue;
            if (func) {
                ostd::err.writefln("redefinition of rule '%s'", tname);
                return 1;
            }
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

                auto dsv = ostd::appender<ostd::String>();
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

ostd::ConstCharRange compare_subst(ostd::ConstCharRange expanded,
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

int exec(cscript::CsState &cs, ostd::ConstCharRange target) {
    ostd::Vector<SubRule> rlist;
    for (auto &rule: rules.iter()) {
        if (target == rule.target) {
            rlist.push().rule = &rule;
            continue;
        }
        ostd::ConstCharRange sub = compare_subst(target, rule.target);
        if (!sub.empty()) {
            SubRule &sr = rlist.push();
            sr.rule = &rule;
            sr.sub = sub;
        }
    }
    if (rlist.empty()) return 0;
    return exec_func(cs, target, rlist);
}

void rule(cscript::CsState &, const char *tgt, const char *dep, uint *body,
          int *numargs) {
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
    cscript::CsState cs;

    cscript::init_lib_base(cs);
    cscript::init_lib_io(cs);
    cscript::init_lib_math(cs);
    cscript::init_lib_string(cs);
    cscript::init_lib_list(cs);

    cs.add_command("shell", "C", [](cscript::CsState &cs, char *s) {
        cs.result->set_int(system(s));
    });

    cs.add_command("rule", "sseN", rule);

    if (!cs.run_file("cubefile", true))
        return 1;
    return exec(cs, (argc > 1) ? argv[1] : "all");
}