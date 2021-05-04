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
#include <ostd/build/make_coroutine.hh>

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
            auto ts = cs.new_thread();
            cs::alias_local target{ts, "target"};
            cs::alias_local source{ts, "source"};
            cs::alias_local sources{ts, "sources"};

            cs::any_value idv{};
            idv.set_string(tgt, ts);
            target.set(std::move(idv));

            if (!srcs.empty()) {

                idv.set_string(srcs[0], ts);
                source.set(std::move(idv));

                auto dsv = ostd::appender<std::string>();
                ostd::format(dsv, "%(%s %)", srcs);
                idv.set_string(dsv.get(), cs);
                sources.set(std::move(idv));
            }

            try {
                body.call(ts);
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
    s.new_command("rule", "ssb", [&mk](auto &css, auto args, auto &) {
        rule_add(
            css, mk, args[0].get_string(css),
            args[1].get_string(css), args[2].get_code()
        );
    });

    s.new_command("action", "sb", [&mk](auto &css, auto args, auto &) {
        rule_add(
            css, mk, args[0].get_string(css), std::string_view{},
            args[1].get_code(), true
        );
    });

    s.new_command("depend", "ss", [&mk](auto &css, auto args, auto &) {
        rule_add(
            css, mk, args[0].get_string(css), args[1].get_string(css),
            cs::bcode_ref{}
        );
    });
}

static void init_baselib(cs::state &s, build::make &mk, bool ignore_env) {
    s.new_command("echo", "...", [](auto &css, auto args, auto &) {
        ostd::writeln(cs::concat_values(css, args, " ").view());
    });

    s.new_command("shell", "...", [&mk](auto &css, auto args, auto &) {
        mk.push_task([
            ds = std::string{cs::concat_values(css, args, " ").view()}
        ]() {
            if (system(ds.data())) {
                throw build::make_error{""};
            }
        });
    });

    s.new_command("getenv", "ss", [ignore_env](auto &css, auto args, auto &res) {
        if (ignore_env) {
            res.set_string("", css);
            return;
        }
        res.set_string(ostd::env_get(
            std::string_view{args[0].get_string(css)}
        ).value_or(
            std::string{std::string_view{args[1].get_string(css)}}
        ), css);
    });

    s.new_command("invoke", "s", [&mk](auto &css, auto args, auto &) {
        mk.exec(std::string_view{args[0].get_string(css)});
    });
}

static void init_pathlib(cs::state &s) {
    s.new_command("extreplace", "sss", [](auto &css, auto args, auto &res) {
        ostd::string_range oldext = std::string_view{args[1].get_string(css)};
        ostd::string_range newext = std::string_view{args[2].get_string(css)};
        std::string ret;
        cs::list_parser p{css, args[0].get_string(css)};
        while (p.parse()) {
            ostd::path np{std::string_view{p.get_item()}};
            if (!ret.empty()) {
                ret += ' ';
            }
            ret += (
                (np.suffixes() == oldext) ? np.with_suffixes(newext) : np
            ).string();
        }
        res.set_string(ret, css);
    });

    s.new_command("glob", "...", [](auto &css, auto args, auto &res) {
        auto app = ostd::appender<std::vector<ostd::path>>();
        cs::list_parser p{css, cs::concat_values(css, args, " ")};
        while (p.parse()) {
            auto it = p.get_item();
            fs::glob_match(app, std::string_view{it});
        }
        res.set_string(ostd::format(
            ostd::appender<std::string>(), "%(%s %)", app.get()
        ).get(), css);
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

    s.compile(std::string_view{buf.get(), std::size_t(len)}, fname).call(s);
    return true;
}

void do_main(int argc, char **argv) {
    /* cubescript interpreter */
    cs::state s;
    cs::std_init_all(s);

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
    s.new_var("numcpus", ncpus, true);
    s.new_var("numjobs", jobs, true);

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

    /* init buildsystem, use coroutine tasks */
    build::make mk{build::make_task_coroutine, jobs};

    /* octabuild cubescript libs */
    init_rulelib(s, mk);
    init_baselib(s, mk, ignore_env);
    init_pathlib(s);

    /* parse rules */
    if ((
        !fcont.empty() && !s.compile(fcont).call(s).get_bool()
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
