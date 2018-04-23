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

struct ob_state: cs_state {
    void rule_add(
        build::make &mk, string_range tgt, string_range dep, cs_bcode *body,
        bool action = false
    ) {
        cscript::util::ListParser p{*this, tgt};
        while (p.parse()) {
            auto &rl = mk.rule(p.get_item()).action(action);
            cscript::util::ListParser lp{*this, dep};
            while (lp.parse()) {
                rl.depend(lp.get_item());
            }
            if (cscript::cs_code_is_empty(body)) {
                continue;
            }
            rl.body([body = cs_bcode_ref(body), this](auto tgt, auto srcs) {
                cs_stacked_value targetv, sourcev, sourcesv;

                if (!targetv.set_alias(new_ident("target"))) {
                    throw build::make_error{
                        "internal error: could not set alias 'target'"
                    };
                }
                targetv.set_cstr(tgt);
                targetv.push();

                if (!srcs.empty()) {
                    if (!sourcev.set_alias(new_ident("source"))) {
                        throw build::make_error{
                            "internal error: could not set alias 'source'"
                        };
                    }
                    if (!sourcesv.set_alias(new_ident("sources"))) {
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
                    this->run(body);
                } catch (cscript::cs_error const &e) {
                    throw build::make_error{e.what()};
                }
            });
        }
    }

    void register_rulecmds(build::make &mk) {
        new_command("rule", "sse", [this, &mk](auto &, auto args, auto &) {
            this->rule_add(
                mk, args[0].get_strr(), args[1].get_strr(), args[2].get_code()
            );
        });

        new_command("action", "se", [this, &mk](auto &, auto args, auto &) {
            this->rule_add(
                mk, args[0].get_strr(), nullptr, args[1].get_code(), true
            );
        });

        new_command("depend", "ss", [this, &mk](auto &, auto args, auto &) {
            this->rule_add(mk, args[0].get_strr(), args[1].get_strr(), nullptr);
        });

    }
};

void do_main(int argc, char **argv) {
    ob_state os;
    os.init_libs();

    bool ignore_env = false;

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
        .action(ostd::arg_store_true(ignore_env));

    std::string action = "default";
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

    if (!jobs) {
        jobs = ncpus;
    }
    jobs = std::max(1, jobs);

    try {
        if (!curdir.empty()) {
            fs::current_path(curdir);
        }
    } catch (fs::fs_error const &e) {
        throw build::make_error{
            "failed changing directory: %s (0s)", curdir, e.what()
        };
    }

    os.new_ivar("numjobs", 4096, 1, jobs);

    build::make mk{build::make_task_simple, jobs};
    os.register_rulecmds(mk);

    os.new_command("echo", "C", [](auto &, auto args, auto &) {
        writeln(args[0].get_strr());
    });

    os.new_command("shell", "C", [&mk](auto &, auto args, auto &) {
        mk.push_task([ds = std::string(args[0].get_strr())]() {
            if (system(ds.data())) {
                throw build::make_error{""};
            }
        });
    });

    os.new_command("getenv", "ss", [ignore_env](auto &, auto args, auto &res) {
        if (ignore_env) {
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

    os.new_command("invoke", "s", [&mk](auto &, auto args, auto &) {
        mk.exec(args[0].get_strr());
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
        throw build::make_error{"failed creating rules"};
    }

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
