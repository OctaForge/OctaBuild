#include <utility>
#include <thread>
#include <unordered_map>
#include <vector>
#include <stack>
#include <queue>
#include <future>
#include <stdexcept>

#include <ostd/string.hh>
#include <ostd/format.hh>
#include <ostd/path.hh>
#include <ostd/io.hh>
#include <ostd/platform.hh>
#include <ostd/environ.hh>
#include <ostd/thread_pool.hh>
#include <ostd/argparse.hh>

#include <cubescript/cubescript.hh>

using ostd::string_range;
using ostd::path;

using cscript::cs_state;
using cscript::cs_value_r;
using cscript::cs_value;
using cscript::cs_stacked_value;
using cscript::cs_bcode_ref;
using cscript::cs_bcode;

namespace fs = ostd::fs;

struct build_error: std::runtime_error {
    using std::runtime_error::runtime_error;

    template<typename ...A>
    build_error(string_range fmt, A const &...args):
        build_error(
            ostd::format(ostd::appender<std::string>(), fmt, args...).get()
        )
    {}
};

/* check funcs */

static bool ob_check_exec(
    string_range tname, std::vector<std::string> const &deps
) {
    if (!fs::exists(tname)) {
        return true;
    }
    for (auto &dep: deps) {
        if (!fs::exists(dep)) {
            return true;
        }
    }
    auto get_ts = [](string_range fname) {
        path p{fname};
        if (!fs::is_regular_file(p)) {
            return fs::file_time_t{};
        }
        return fs::last_write_time(p);
    };
    auto tts = get_ts(tname);
    if (tts == fs::file_time_t{}) {
        return true;
    }
    for (auto &dep: deps) {
        auto sts = get_ts(dep);
        if ((sts != fs::file_time_t{}) && (tts < sts)) {
            return true;
        }
    }
    return false;
}

/* this lets us properly match % patterns in target names */
static string_range ob_compare_subst(
    string_range expanded, string_range toexpand
) {
    auto rep = ostd::find(toexpand, '%');
    /* no subst found */
    if (rep.empty()) {
        return nullptr;
    }
    /* get the part before % */
    auto fp = toexpand.slice(0, &rep[0] - &toexpand[0]);
    /* part before % does not compare, so ignore */
    if (expanded.size() <= fp.size()) {
        return nullptr;
    }
    if (expanded.slice(0, fp.size()) != fp) {
        return nullptr;
    }
    /* pop out front part */
    expanded = expanded.slice(fp.size(), expanded.size());
    /* part after % */
    ++rep;
    if (rep.empty()) {
        return expanded;
    }
    /* part after % does not compare, so ignore */
    if (expanded.size() <= rep.size()) {
        return nullptr;
    }
    size_t es = expanded.size();
    if (expanded.slice(es - rep.size(), es) != rep) {
        return nullptr;
    }
    /* cut off latter part */
    expanded = expanded.slice(0, expanded.size() - rep.size());
    /* we got what we wanted... */
    return expanded;
}

struct ob_state: cs_state {
    bool ignore_env = false;

    /* represents a rule definition, possibly with a function */
    struct Rule {
        std::string target;
        std::vector<std::string> deps;
        cs_bcode_ref func;
        bool action;

        Rule(): target(), deps(), func(), action(false) {}
        Rule(Rule const &r):
            target(r.target), deps(r.deps), func(r.func), action(r.action)
        {}
    };

    std::vector<Rule> rules;

    struct SubRule {
        string_range sub;
        Rule *rule;
    };

    std::unordered_map<string_range, std::vector<SubRule>> cache;

    ostd::thread_pool tpool;
    std::stack<std::queue<std::future<void>> *> waiting;

    template<typename F>
    void wait_result(F func) {
        std::queue<std::future<void>> waits;
        waiting.push(&waits);
        try {
            func();
        } catch (...) {
            waiting.pop();
            throw;
        }
        waiting.pop();
        for (; !waits.empty(); waits.pop()) {
            try {
                waits.front().get();
            } catch (build_error const &) {
                waits.pop();
                ostd::writeln("waiting for the remaining tasks to finish...");
                for (; !waits.empty(); waits.pop()) {
                    try {
                        waits.front().get();
                    } catch (build_error const &) {
                        /* no rethrow */
                    }
                }
                throw;
            }
        }
    }

    template<typename F>
    void push_task(F &&func) {
        waiting.top()->push(tpool.push(std::forward<F>(func)));
    }

    void exec_list(
        std::vector<SubRule> const &rlist, std::vector<std::string> &subdeps,
        string_range tname
    ) {
        std::string repd;
        for (auto &sr: rlist) {
            for (auto &target: sr.rule->deps) {
                string_range atgt = ostd::iter(target);
                repd.clear();
                auto lp = ostd::find(atgt, '%');
                if (!lp.empty()) {
                    repd.append(atgt.slice(0, &lp[0] - &atgt[0]));
                    repd.append(sr.sub);
                    ++lp;
                    if (!lp.empty()) {
                        repd.append(lp);
                    }
                    atgt = ostd::iter(repd);
                }
                subdeps.push_back(std::string{atgt});
                exec_rule(atgt, tname);
            }
        }
    }

    void exec_func(string_range tname, std::vector<SubRule> const &rlist) {
        std::vector<std::string> subdeps;
        wait_result([&rlist, &subdeps, &tname, this]() {
            exec_list(rlist, subdeps, tname);
        });
        cs_bcode_ref *func = nullptr;
        bool act = false;
        for (auto &sr: rlist) {
            if (sr.rule->func) {
                func = &sr.rule->func;
                act = sr.rule->action;
                break;
            }
        }
        if ((act || ob_check_exec(tname, subdeps)) && func) {
            cs_stacked_value targetv, sourcev, sourcesv;

            if (!targetv.set_alias(new_ident("target"))) {
                throw build_error{
                    "internal error: could not set alias 'target'"
                };
            }
            targetv.set_cstr(tname);
            targetv.push();

            if (!subdeps.empty()) {
                if (!sourcev.set_alias(new_ident("source"))) {
                    throw build_error{
                        "internal error: could not set alias 'source'"
                    };
                }
                if (!sourcesv.set_alias(new_ident("sources"))) {
                    throw build_error{
                        "internal error: could not set alias 'sources'"
                    };
                }

                sourcev.set_cstr(subdeps[0]);
                sourcev.push();

                auto dsv = ostd::appender<std::string>();
                ostd::format(dsv, "%(%s %)", subdeps);
                sourcesv.set_str(std::move(dsv.get()));
                sourcesv.push();
            }

            try {
                run(*func);
            } catch (cscript::cs_error const &e) {
                throw build_error{e.what()};
            }
        }
    }

    void exec_action(Rule *rule) {
        try {
            run(rule->func);
        } catch (cscript::cs_error const &e) {
            throw build_error{e.what()};
        }
    }

    void find_rules(string_range target, std::vector<SubRule> &rlist) {
        if (!rlist.empty()) {
            return;
        }
        SubRule *frule = nullptr;
        bool exact = false;
        for (auto &rule: rules) {
            if (target == string_range{rule.target}) {
                rlist.emplace_back();
                rlist.back().rule = &rule;
                if (rule.func) {
                    if (frule && exact) {
                        throw build_error{"redefinition of rule '%s'", target};
                    }
                    if (!frule) {
                        frule = &rlist.back();
                    } else {
                        *frule = rlist.back();
                        rlist.pop_back();
                    }
                    exact = true;
                }
                continue;
            }
            if (exact || !rule.func) {
                continue;
            }
            string_range sub = ob_compare_subst(target, rule.target);
            if (!sub.empty()) {
                rlist.emplace_back();
                SubRule &sr = rlist.back();
                sr.rule = &rule;
                sr.sub = sub;
                if (frule) {
                    if (sub.size() == frule->sub.size()) {
                        throw build_error{"redefinition of rule '%s'", target};
                    }
                    if (sub.size() < frule->sub.size()) {
                        *frule = sr;
                        rlist.pop_back();
                    }
                } else {
                    frule = &sr;
                }
            }
        }
    }

    void exec_rule(string_range target, string_range from = nullptr) {
        std::vector<SubRule> &rlist = cache[target];
        find_rules(target, rlist);
        if ((rlist.size() == 1) && rlist[0].rule->action) {
            exec_action(rlist[0].rule);
            return;
        }
        if (rlist.empty() && !fs::exists(target)) {
            if (from.empty()) {
                throw build_error{"no rule to run target '%s'", target};
            } else {
                throw build_error{
                    "no rule to run target '%s' (needed by '%s')", target, from
                };
            }
        }
        exec_func(target, rlist);
    }

    void exec_main(string_range target) {
        wait_result([&target, this]() { exec_rule(target); });
    }

    void rule_add(
        cs_state &cs, string_range tgt, string_range dep, cs_bcode *body,
        bool action = false
    ) {
        cscript::util::ListParser p{cs, tgt};
        while (p.parse()) {
            rules.emplace_back();
            Rule &r = rules.back();
            r.target = p.get_item();
            r.action = action;
            r.func = cscript::cs_code_is_empty(body) ? nullptr : body;
            cscript::util::ListParser lp{cs, dep};
            while (lp.parse()) {
                r.deps.push_back(lp.get_item());
            }
        }
    }

    void rule_dup(
        cs_state &cs, string_range tgt, string_range ptgt,
        string_range dep, bool inherit_deps
    ) {
        Rule *oldr = nullptr;
        for (auto &rule: rules) {
            if (ptgt == string_range{rule.target}) {
                oldr = &rule;
                break;
            }
        }
        if (!oldr) {
            return;
        }
        rules.emplace_back();
        Rule &r = rules.back();
        r.target = tgt;
        r.action = oldr->action;
        r.func = oldr->func;
        if (inherit_deps) {
            r.deps = oldr->deps;
        } else {
            cscript::util::ListParser p{cs, dep};
            while (p.parse()) {
                r.deps.push_back(p.get_item());
            }
        }
    }

    void register_rulecmds() {
        new_command("rule", "sse", [this](auto &cs, auto args, auto &) {
            this->rule_add(
                cs, args[0].get_strr(), args[1].get_strr(), args[2].get_code()
            );
        });

        new_command("action", "se", [this](auto &cs, auto args, auto &) {
            this->rule_add(
                cs, args[0].get_strr(), nullptr, args[1].get_code(), true
            );
        });

        new_command("depend", "ss", [this](auto &cs, auto args, auto &) {
            this->rule_add(cs, args[0].get_strr(), args[1].get_strr(), nullptr);
        });

        new_command("duprule", "sssN", [this](auto &cs, auto args, auto &) {
            this->rule_dup(
                cs, args[0].get_strr(), args[1].get_strr(),
                args[2].get_strr(), args[3].get_int() <= 2
            );
        });
    }
};

void do_main(int argc, char **argv) {
    ob_state os;
    os.init_libs();

    int ncpus = std::thread::hardware_concurrency();
    os.new_ivar("numcpus", 4096, 1, ncpus);

    ostd::arg_parser ap;

    auto &help = ap.add_optional("-h", "--help", 0)
        .help("print this message and exit")
        .action(ostd::arg_print_help(ap));

    int jobs = 1;
    ap.add_optional("-j", "--jobs", 1)
        .help("specify the number of jobs to use (default: 1)")
        .action(ostd::arg_store_format("%d", jobs));

    std::string curdir;
    ap.add_optional("-C", "--change-directory", 1)
        .help("change to DIRECTORY before running")
        .metavar("DIRECTORY")
        .action(ostd::arg_store_str(curdir));

    std::string deffile = "obuild.cfg";
    ap.add_optional("-f", "--file", 1)
        .help("specify the file to run (default: obuild.cfg)")
        .action(ostd::arg_store_str(deffile));

    std::string fcont;
    ap.add_optional("-e", "--execute", 1)
        .help("evaluate a string instead of a file")
        .metavar("STR")
        .action(ostd::arg_store_str(fcont));

    ap.add_optional("-E", "--ignore-env", 0)
        .help("ignore environment variables")
        .action(ostd::arg_store_true(os.ignore_env));

    std::string action = "default";
    ap.add_positional("action", ostd::arg_value::OPTIONAL)
        .help("the action to perform")
        .action(ostd::arg_store_str(action));

    try {
        ap.parse(argc, argv);
    } catch (ostd::arg_error const &e) {
        ostd::cerr.writefln("%s: %s", argv[0], e.what());
        ap.print_help(ostd::cerr.iter());
        throw build_error{""};
    }

    if (help.used()) {
        return;
    }

    if (!jobs) {
        jobs = ncpus;
    }
    jobs = std::max(1, jobs);

    try {
        if (!curdir.empty()) {
            fs::current_path(curdir);
        }
    } catch (fs::fs_error const &e) {
        throw build_error{
            "failed changing directory: %s (0s)", curdir, e.what()
        };
    }

    os.new_ivar("numjobs", 4096, 1, jobs);

    os.tpool.start(jobs);

    os.register_rulecmds();

    os.new_command("echo", "C", [](auto &, auto args, auto &) {
        writeln(args[0].get_strr());
    });

    os.new_command("shell", "C", [&os](auto &, auto args, auto &) {
        os.push_task([ds = std::string(args[0].get_strr())]() {
            if (system(ds.data())) {
                throw build_error{""};
            }
        });
    });

    os.new_command("getenv", "ss", [&os](auto &, auto args, auto &res) {
        if (os.ignore_env) {
            res.set_cstr("");
            return;
        }
        res.set_str(std::move(
            ostd::env_get(args[0].get_str()).value_or(args[1].get_str())
        ));
    });

    os.new_command("extreplace", "sss", [](auto &cs, auto args, auto &res) {
        string_range lst = args[0].get_strr();
        string_range oldext = args[1].get_strr();
        string_range newext = args[2].get_strr();
        std::string ret;
        if (oldext.front() == '.') {
            oldext.pop_front();
        }
        if (newext.front() == '.') {
            newext.pop_front();
        }
        cscript::util::ListParser p{cs, lst};
        while (p.parse()) {
            auto elem = p.get_item();
            string_range it = ostd::iter(elem);
            if (!ret.empty()) {
                ret += ' ';
            }
            auto dot = ostd::find_last(it, '.');
            if (!dot.empty() && (dot.slice(1, dot.size()) == oldext)) {
                ret += it.slice(0, &dot[0] - &it[0]);
                ret += '.';
                ret += newext;
            } else {
                ret += it;
            }
        }
        res.set_str(std::move(ret));
    });

    os.new_command("invoke", "s", [&os](auto &, auto args, auto &) {
        os.exec_main(args[0].get_strr());
    });

    os.new_command("glob", "C", [](auto &cs, auto args, auto &res) {
        auto ret = ostd::appender<std::string>();
        auto app = ostd::appender<std::vector<path>>();
        cscript::util::ListParser p{cs, args[0].get_strr()};
        while (p.parse()) {
            fs::glob_match(app, p.get_item());
        }
        ostd::format(ret, "%(%s %)", app.get());
        res.set_str(std::move(ret.get()));
    });

    if ((!fcont.empty() && !os.run_bool(fcont)) || !os.run_file(deffile)) {
        throw build_error{"failed creating rules"};
    }

    if (os.rules.empty()) {
        throw build_error{"no targets"};
    }

    os.exec_main(action);
}

int main(int argc, char **argv) {
    try {
        do_main(argc, argv);
    } catch (build_error const &e) {
        auto s = e.what();
        if (s[0]) {
            ostd::cerr.writefln("%s: %s", argv[0], s);
        }
        return 1;
    }
    return 0;
}
