#include <utility>
#include <vector>
#include <stdexcept>

#include <ostd/string.hh>
#include <ostd/format.hh>
#include <ostd/path.hh>
#include <ostd/io.hh>
#include <ostd/platform.hh>
#include <ostd/environ.hh>
#include <ostd/argparse.hh>

#include <ostd/build/make.hh>

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
namespace build = ostd::build;

static void rule_add(
    cs_state &cs, build::make &mk,
    string_range target, string_range depends,
    cs_bcode *body, bool action = false
) {
    cscript::util::ListParser p{cs, target};
    while (p.parse()) {
        auto &rl = mk.rule(p.get_item()).action(action);
        cscript::util::ListParser lp{cs, depends};
        while (lp.parse()) {
            rl.depend(lp.get_item());
        }
        if (cscript::cs_code_is_empty(body)) {
            continue;
        }
        rl.body([body = cs_bcode_ref(body), &cs](auto tgt, auto srcs) {
            cs_stacked_value targetv, sourcev, sourcesv;

            if (!targetv.set_alias(cs.new_ident("target"))) {
                throw build::make_error{
                    "internal error: could not set alias 'target'"
                };
            }
            targetv.set_cstr(tgt);
            targetv.push();

            if (!srcs.empty()) {
                if (!sourcev.set_alias(cs.new_ident("source"))) {
                    throw build::make_error{
                        "internal error: could not set alias 'source'"
                    };
                }
                if (!sourcesv.set_alias(cs.new_ident("sources"))) {
                    throw build::make_error{
                        "internal error: could not set alias 'sources'"
                    };
                }

                sourcev.set_cstr(srcs[0]);
                sourcev.push();

                auto dsv = ostd::appender<std::string>();
                ostd::format(dsv, "%(%s %)", srcs);
                sourcesv.set_str(std::move(dsv.get()));
                sourcesv.push();
            }

            try {
                cs.run(body);
            } catch (cscript::cs_error const &e) {
                throw build::make_error{e.what()};
            }
        });
    }
}

static void init_rulelib(cs_state &cs, build::make &mk) {
    cs.new_command("rule", "sse", [&cs, &mk](auto &, auto args, auto &) {
        rule_add(
            cs, mk, args[0].get_strr(), args[1].get_strr(), args[2].get_code()
        );
    });

    cs.new_command("action", "se", [&cs, &mk](auto &, auto args, auto &) {
        rule_add(
            cs, mk, args[0].get_strr(), nullptr, args[1].get_code(), true
        );
    });

    cs.new_command("depend", "ss", [&cs, &mk](auto &, auto args, auto &) {
        rule_add(cs, mk, args[0].get_strr(), args[1].get_strr(), nullptr);
    });
}

static void init_baselib(cs_state &cs, build::make &mk, bool ignore_env) {
    cs.new_command("echo", "C", [](auto &, auto args, auto &) {
        writeln(args[0].get_strr());
    });

    cs.new_command("shell", "C", [&mk](auto &, auto args, auto &) {
        mk.push_task([ds = std::string(args[0].get_strr())]() {
            if (system(ds.data())) {
                throw build::make_error{""};
            }
        });
    });

    cs.new_command("getenv", "ss", [ignore_env](auto &, auto args, auto &res) {
        if (ignore_env) {
            res.set_cstr("");
            return;
        }
        res.set_str(std::move(
            ostd::env_get(args[0].get_str()).value_or(args[1].get_str())
        ));
    });

    cs.new_command("invoke", "s", [&mk](auto &, auto args, auto &) {
        mk.exec(args[0].get_strr());
    });
}

static void init_pathlib(cs_state &cs) {
    cs.new_command("extreplace", "sss", [](auto &cs, auto args, auto &res) {
        string_range oldext = args[1].get_strr();
        string_range newext = args[2].get_strr();
        std::string ret;
        for (cscript::util::ListParser p{cs, args[0].get_strr()}; p.parse();) {
            ostd::path np{p.get_item()};
            if (!ret.empty()) {
                ret += ' ';
            }
            ret += (
                (np.suffixes() == oldext) ? np.with_suffixes(newext) : np
            ).string();
        }
        res.set_str(std::move(ret));
    });

    cs.new_command("glob", "C", [](auto &cs, auto args, auto &res) {
        auto ret = ostd::appender<std::string>();
        auto app = ostd::appender<std::vector<path>>();;
        for (cscript::util::ListParser p{cs, args[0].get_strr()}; p.parse();) {
            fs::glob_match(app, p.get_item());
        }
        ostd::format(ret, "%(%s %)", app.get());
        res.set_str(std::move(ret.get()));
    });
}

void do_main(int argc, char **argv) {
    /* cubescript interpreter */
    cs_state cs;
    cs.init_libs();

    /* arg values */
    std::string action  = "default";
    std::string deffile = "obuild.cfg";
    std::string curdir;
    std::string fcont;
    bool ignore_env = false;
    int jobs = 1;

    /* input options */
    {
        ostd::arg_parser ap;

        auto &help = ap.add_optional("-h", "--help", 0)
            .help("print this message and exit")
            .action(ostd::arg_print_help(ap));

        ap.add_optional("-j", "--jobs", 1)
            .help("specify the number of jobs to use (default: 1)")
            .action(ostd::arg_store_format("%d", jobs));

        ap.add_optional("-C", "--change-directory", 1)
            .help("change to DIRECTORY before running")
            .metavar("DIRECTORY")
            .action(ostd::arg_store_str(curdir));

        ap.add_optional("-f", "--file", 1)
            .help("specify the file to run (default: obuild.cfg)")
            .action(ostd::arg_store_str(deffile));

        ap.add_optional("-e", "--execute", 1)
            .help("evaluate a string instead of a file")
            .metavar("STR")
            .action(ostd::arg_store_str(fcont));

        ap.add_optional("-E", "--ignore-env", 0)
            .help("ignore environment variables")
            .action(ostd::arg_store_true(ignore_env));

        ap.add_positional("action", ostd::arg_value::OPTIONAL)
            .help("the action to perform")
            .action(ostd::arg_store_str(action));

        try {
            ap.parse(argc, argv);
        } catch (ostd::arg_error const &e) {
            ostd::cerr.writefln("%s: %s", argv[0], e.what());
            ap.print_help(ostd::cerr.iter());
            throw build::make_error{""};
        }

        if (help.used()) {
            return;
        }
    }

    int ncpus = std::thread::hardware_concurrency();
    jobs = std::max(1, jobs ? jobs : ncpus);

    /* core cubescript variables */
    cs.new_ivar("numcpus", 4096, 1, ncpus);
    cs.new_ivar("numjobs", 4096, 1, jobs);

    /* switch to target directory */
    try {
        if (!curdir.empty()) {
            fs::current_path(curdir);
        }
    } catch (fs::fs_error const &e) {
        throw build::make_error{
            "failed changing directory: %s (%s)", curdir, e.what()
        };
    }

    /* init buildsystem, use simple tasks, cubescript cannot into coroutines */
    build::make mk{build::make_task_simple, jobs};

    /* octabuild cubescript libs */
    init_rulelib(cs, mk);
    init_baselib(cs, mk, ignore_env);
    init_pathlib(cs);

    /* parse rules */
    if ((!fcont.empty() && !cs.run_bool(fcont)) || !cs.run_file(deffile)) {
        throw build::make_error{"failed creating rules"};
    }

    /* make */
    mk.exec(action);
}

int main(int argc, char **argv) {
    try {
        do_main(argc, argv);
    } catch (build::make_error const &e) {
        auto s = e.what();
        if (s[0]) {
            ostd::cerr.writefln("%s: %s", argv[0], s);
        }
        return 1;
    }
    return 0;
}
