#include <ostd/types.hh>
#include <ostd/string.hh>
#include <ostd/vector.hh>
#include <ostd/filesystem.hh>
#include <ostd/io.hh>

#include <cubescript.hh>

using ostd::ConstCharRange;
using ostd::Vector;
using ostd::String;
using ostd::slice_until;

using cscript::CsState;
using cscript::TvalRange;

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

static String ob_expand_globs(Vector<String> const &src) {
    String ret;
    for (auto &s: src.iter()) {
        ob_expand_glob(ret, s.iter());
    }
    return ret;
}

void cs_register_globs(CsState &cs) {
    cs.add_command("glob", "C", [&cs](TvalRange args) {
        auto fnames = cscript::util::list_explode(args[0].get_strr());
        cs.result->set_str(ob_expand_globs(fnames).disown());
    });
}
