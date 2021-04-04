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

namespace cs = cubescript;
namespace fs = ostd::fs;
namespace build = ostd::build;

static void rule_add(
    cs::state &cs, build::make &mk,
    std::string_view target, std::string_view depends,
    cs::bcode_ref body, bool action = false
) {
    build::make_rule::body_func bodyf{};
    if (!body.empty()) {
        bodyf = [body, &cs](auto tgt, auto srcs) {
            cs::alias_local target{cs, cs.new_ident("target")};
            cs::alias_local source{cs, cs.new_ident("source")};
            cs::alias_local sources{cs, cs.new_ident("sources")};
            if (!target) {
                throw build::make_error{
                    "internal error: could not set alias 'target'"
                };
            }

            cs::any_value idv{cs};
            idv.set_str(tgt);
            target.set(std::move(idv));

            if (!srcs.empty()) {
                if (!source) {
                    throw build::make_error{
                        "internal error: could not set alias 'source'"
                    };
                }
                if (!sources) {
                    throw build::make_error{
                        "internal error: could not set alias 'sources'"
                    };
                }

                idv.set_str(srcs[0]);
                source.set(std::move(idv));

                auto dsv = ostd::appender<std::string>();
                ostd::format(dsv, "%(%s %)", srcs);
                idv.set_str(dsv.get());
                sources.set(std::move(idv));
            }

            try {
                cs.run(body);
            } catch (cs::error const &e) {
                throw build::make_error{e.what()};
            }
        };
    }
    cs::list_parser p{cs, target};
    while (p.parse()) {
        cs::list_parser lp{cs, depends};
        auto &r = mk.rule(
            std::string_view{p.get_item()}
        ).action(action).body(bodyf);
        while (lp.parse()) {
            r.depend(std::string_view{lp.get_item()});
        }
    }
}

static void init_rulelib(cs::state &s, build::make &mk) {
    s.new_command("rule", "sse", [&mk](auto &css, auto args, auto &) {
        rule_add(
            css, mk, args[0].get_str(), args[1].get_str(), args[2].get_code()
        );
    });

    s.new_command("action", "se", [&mk](auto &css, auto args, auto &) {
        rule_add(
            css, mk, args[0].get_str(), std::string_view{},
            args[1].get_code(), true
        );
    });

    s.new_command("depend", "ss", [&mk](auto &css, auto args, auto &) {
        rule_add(css, mk, args[0].get_str(), args[1].get_str(), nullptr);
    });
}

static void init_baselib(cs::state &s, build::make &mk, bool ignore_env) {
    s.new_command("echo", "C", [](auto &, auto args, auto &) {
        ostd::writeln(std::string_view{args[0].get_str()});
    });

    s.new_command("shell", "C", [&mk](auto &, auto args, auto &) {
        mk.push_task([ds = std::string(std::string_view{args[0].get_str()})]() {
            if (system(ds.data())) {
                throw build::make_error{""};
            }
        });
    });

    s.new_command("getenv", "ss", [ignore_env](auto &, auto args, auto &res) {
        if (ignore_env) {
            res.set_str("");
            return;
        }
        res.set_str(ostd::env_get(std::string_view{args[0].get_str()}).value_or(
            std::string{std::string_view{args[1].get_str()}}
        ));
    });

    s.new_command("invoke", "s", [&mk](auto &, auto args, auto &) {
        mk.exec(std::string_view{args[0].get_str()});
    });
}

static void init_pathlib(cs::state &s) {
    s.new_command("extreplace", "sss", [](auto &css, auto args, auto &res) {
        ostd::string_range oldext = std::string_view{args[1].get_str()};
        ostd::string_range newext = std::string_view{args[2].get_str()};
        std::string ret;
        cs::list_parser p{css, args[0].get_str()};
        while (p.parse()) {
            ostd::path np{std::string_view{p.get_item()}};
            if (!ret.empty()) {
                ret += ' ';
            }
            ret += (
                (np.suffixes() == oldext) ? np.with_suffixes(newext) : np
            ).string();
        }
        res.set_str(ret);
    });

    s.new_command("glob", "C", [](auto &css, auto args, auto &res) {
        auto app = ostd::appender<std::vector<ostd::path>>();
        cs::list_parser p{css, args[0].get_str()};
        while (p.parse()) {
            auto it = p.get_item();
            fs::glob_match(app, std::string_view{it});
        }
        res.set_str(ostd::format(
            ostd::appender<std::string>(), "%(%s %)", app.get()
        ).get());
    });
}

static bool do_run_file(cs::state &s, std::string_view fname) {
    ostd::file_stream f{fname};
    if (!f.is_open()) {
        return false;
    }

    f.seek(0, ostd::stream_seek::END);
    auto len = f.tell();
    f.seek(0);

    auto buf = std::make_unique<char[]>(len + 1);
    if (!buf) {
        return false;
    }

    if (f.read_bytes(buf.get(), len) != std::size_t(len)) {
        return false;
    }

    buf[len] = '\0';

    s.run(std::string_view{buf.get(), std::size_t(len)}, fname);
    return true;
}

void do_main(int argc, char **argv) {
    /* cubescript interpreter */
    cs::state s;
    s.init_libs();

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
    s.new_ivar("numcpus", ncpus, true);
    s.new_ivar("numjobs", jobs, true);

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
    init_rulelib(s, mk);
    init_baselib(s, mk, ignore_env);
    init_pathlib(s);

    /* parse rules */
    if ((
        !fcont.empty() && !s.run(fcont).get_bool()
    ) || !do_run_file(s, deffile)) {
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
