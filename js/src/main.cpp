// kjs: run scripts, or run test262 tests in batch.
//   kjs file.js ...
//   kjs --test262 <test262 dir>   (test paths on stdin, one result line per test on stdout)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "interp.h"

using namespace kjs;

static std::string read_file(const std::string& p, bool* ok = nullptr) {
    std::ifstream f(p, std::ios::binary);
    if (ok) *ok = bool(f);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string describe(Runtime& rt, Value v) {
    try {
        if (v.is_obj()) {
            Value n = rt.get(v, "name"), m = rt.get(v, "message");
            if (n.is_str()) return to_utf8(n.s->v) + ": " + (m.is_undef() ? "" : to_utf8(rt.to_string(m)->v));
        }
        return to_utf8(rt.to_string(v)->v);
    } catch (Throw&) {
        return "<unprintable exception>";
    }
}
static std::string error_name(Runtime& rt, Value v) {
    try {
        if (v.is_obj()) {
            Value c = rt.get(v, "constructor");
            if (c.is_obj()) {
                Value n = rt.get(c, "name");
                if (n.is_str()) return to_utf8(n.s->v);
            }
        }
    } catch (Throw&) {
    }
    return "";
}

// compiles a script; on a syntax error returns null and sets *err
static Code* compile_script(Runtime& rt, const u16& src, const std::string& name, std::string* err) {
    Ast ast;
    ast.source = src;
    ParseOptions o;
    try {
        parse(ast, o);
        return compile_program(rt, ast, name, false, false);
    } catch (SyntaxError& e) {
        *err = e.msg + " (line " + std::to_string(e.line) + ")";
        return nullptr;
    }
}
static Value run_script(Runtime& rt, Code* c) {
    Frame* f = rt.alloc<Frame>();
    f->code = c;
    f->env = rt.realm.global_env;
    rt.frames.push_back(f);
    struct Pop {
        Runtime& r;
        ~Pop() { r.frames.pop_back(); }
    } p{rt};
    return execute(rt, f, Resume{}).v;
}

struct Meta {
    std::vector<std::string> includes, flags, features;
    std::string neg_phase, neg_type;
};
static Meta parse_meta(const std::string& src) {
    Meta m;
    size_t a = src.find("/*---"), b = src.find("---*/");
    if (a == std::string::npos || b == std::string::npos) return m;
    std::string y = src.substr(a + 5, b - a - 5);
    std::istringstream in(y);
    std::string line, section;
    auto list = [](const std::string& s) {
        std::vector<std::string> out;
        size_t l = s.find('['), r = s.rfind(']');
        if (l == std::string::npos || r == std::string::npos) return out;
        std::stringstream ss(s.substr(l + 1, r - l - 1));
        std::string item;
        while (std::getline(ss, item, ',')) {
            size_t x = item.find_first_not_of(" \t"), z = item.find_last_not_of(" \t\r");
            if (x != std::string::npos) out.push_back(item.substr(x, z - x + 1));
        }
        return out;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = line;
        size_t x = t.find_first_not_of(" ");
        if (x == std::string::npos) continue;
        bool indented = x > 0;
        t = t.substr(x);
        if (!indented) section.clear();
        if (t.rfind("includes:", 0) == 0) {
            m.includes = list(t);
            if (m.includes.empty()) section = "includes";
        } else if (t.rfind("flags:", 0) == 0) {
            m.flags = list(t);
        } else if (t.rfind("features:", 0) == 0) {
            m.features = list(t);
            if (m.features.empty()) section = "features";
        } else if (t.rfind("negative:", 0) == 0) {
            section = "negative";
        } else if (section == "negative" && t.rfind("phase:", 0) == 0) {
            m.neg_phase = t.substr(6);
            m.neg_phase.erase(0, m.neg_phase.find_first_not_of(" "));
        } else if (section == "negative" && t.rfind("type:", 0) == 0) {
            m.neg_type = t.substr(5);
            m.neg_type.erase(0, m.neg_type.find_first_not_of(" "));
        } else if ((section == "includes" || section == "features") && t.rfind("- ", 0) == 0) {
            (section == "includes" ? m.includes : m.features).push_back(t.substr(2));
        }
    }
    return m;
}

static std::map<std::string, std::string> harness_cache;

// runs one test in one mode; returns "" on pass or a failure reason
static std::string run_one(const std::string& root, const std::string& src, const Meta& meta, bool strict) {
    Runtime rt;
    std::string printed;
    rt.print = [&](const u16& s) { printed += to_utf8(s) + "\n"; };
    bool raw = std::find(meta.flags.begin(), meta.flags.end(), "raw") != meta.flags.end();
    bool async = std::find(meta.flags.begin(), meta.flags.end(), "async") != meta.flags.end();
    // a watchdog
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::thread dog([&] {
        std::unique_lock<std::mutex> l(mu);
        if (!cv.wait_for(l, std::chrono::seconds(5), [&] { return done; })) rt.interrupt = true;
    });
    struct Join {
        std::thread& t;
        std::mutex& m;
        std::condition_variable& c;
        bool& d;
        ~Join() {
            {
                std::lock_guard<std::mutex> l(m);
                d = true;
            }
            c.notify_all();
            t.join();
        }
    } join{dog, mu, cv, done};

    std::vector<std::string> inc;
    if (!raw) inc = {"assert.js", "sta.js"};
    if (async) inc.push_back("doneprintHandle.js");
    for (auto& i : meta.includes) inc.push_back(i);
    try {
        for (auto& h : inc) {
            auto it = harness_cache.find(h);
            if (it == harness_cache.end()) {
                bool ok;
                std::string text = read_file(root + "/harness/" + h, &ok);
                if (!ok) return "missing harness file " + h;
                it = harness_cache.emplace(h, text).first;
            }
            std::string err;
            Code* c = compile_script(rt, to_u16(it->second), h, &err);
            if (!c) return "harness " + h + " failed to parse: " + err;
            run_script(rt, c);
        }
    } catch (Throw& t) {
        return "harness threw " + describe(rt, t.v);
    }
    std::string body = strict ? "\"use strict\";\n" + src : src;
    std::string err;
    Code* c = compile_script(rt, to_u16(body), "test", &err);
    if (!c) {
        if (meta.neg_phase == "parse" && meta.neg_type == "SyntaxError") return "";
        return "SyntaxError: " + err;
    }
    if (meta.neg_phase == "parse" || meta.neg_phase == "resolution") return "expected a parse-time " + meta.neg_type;
    try {
        run_script(rt, c);
        rt.run_jobs();
    } catch (Throw& t) {
        if (meta.neg_phase == "runtime") {
            std::string n = error_name(rt, t.v);
            if (n == meta.neg_type) return "";
            return "expected " + meta.neg_type + ", got " + describe(rt, t.v);
        }
        return describe(rt, t.v);
    } catch (std::exception& e) {
        return std::string("internal error: ") + e.what();
    }
    if (!meta.neg_phase.empty()) return "expected a runtime " + meta.neg_type;
    if (async) {
        if (printed.find("Test262:AsyncTestComplete") != std::string::npos) return "";
        size_t p = printed.find("Test262:AsyncTestFailure:");
        if (p != std::string::npos) return printed.substr(p, printed.find('\n', p) - p);
        return "async test did not complete";
    }
    return "";
}

static int test262(const std::string& root) {
    std::string path;
    while (std::getline(std::cin, path)) {
        if (!path.empty() && path.back() == '\r') path.pop_back();
        if (path.empty()) continue;
        bool ok;
        std::string src = read_file(root + "/" + path, &ok);
        std::string reason;
        if (!ok) reason = "cannot read";
        else {
            Meta m = parse_meta(src);
            bool only_strict = std::find(m.flags.begin(), m.flags.end(), "onlyStrict") != m.flags.end();
            bool no_strict = std::find(m.flags.begin(), m.flags.end(), "noStrict") != m.flags.end() ||
                             std::find(m.flags.begin(), m.flags.end(), "raw") != m.flags.end();
            bool module = std::find(m.flags.begin(), m.flags.end(), "module") != m.flags.end();
            if (module) reason = "modules are not supported";
            else {
                if (!only_strict) reason = run_one(root, src, m, false);
                if (reason.empty() && !no_strict) {
                    std::string r = run_one(root, src, m, true);
                    if (!r.empty()) reason = "(strict) " + r;
                }
            }
        }
        for (char& ch : reason)
            if (ch == '\n' || ch == '\r') ch = ' ';
        std::cout << (reason.empty() ? "PASS " : "FAIL ") << path << (reason.empty() ? "" : " :: " + reason) << "\n" << std::flush;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--test262") return test262(argv[2]);
    if (argc < 2) {
        fprintf(stderr, "usage: kjs file.js ... | kjs --test262 <dir> < paths\n");
        return 2;
    }
    Runtime rt;
    rt.print = [](const u16& s) { std::cout << to_utf8(s) << "\n"; };
    for (int i = 1; i < argc; i++) {
        bool ok;
        std::string src = read_file(argv[i], &ok);
        if (!ok) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        std::string err;
        Code* c = compile_script(rt, to_u16(src), argv[i], &err);
        if (!c) {
            fprintf(stderr, "SyntaxError: %s\n", err.c_str());
            return 1;
        }
        try {
            run_script(rt, c);
            rt.run_jobs();
        } catch (Throw& t) {
            fprintf(stderr, "Uncaught %s\n", describe(rt, t.v).c_str());
            return 1;
        }
    }
    return 0;
}
