// The package manager: ixo init / add / install / update / remove.
//
// Dependencies live in ixo.toml:
//     [dependencies]
//     json5 = { git = "https://github.com/someone/json5.git", rev = "v1.2" }
//     shared = { path = "../shared" }
// `ixo install` puts every package (and the packages they depend on) flat in
// ixo_modules/<name>/ and records the exact commits in ixo.lock, so the next
// install gets the same code. `ixo update` moves to the newest commits.
#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common.hpp"
#include "process.hpp"
#include "toml.hpp"
#include "tools.hpp"

namespace ixo {

namespace {

namespace fs = std::filesystem;

fs::path P(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string S(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// One spelling per repository: "file:///C:/x/repo.git", "C:\x\repo" and
// "C:/x/repo/" are the same place.
std::string sameRepoKey(std::string s) {
    if (s.rfind("file://", 0) == 0) {
        s = s.substr(7);
        if (s.size() > 3 && s[0] == '/' && s[2] == ':') s = s.substr(1);
    }
    std::replace(s.begin(), s.end(), '\\', '/');
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".git") == 0) s.resize(s.size() - 4);
#ifdef _WIN32
    if (s.find("://") == std::string::npos)
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
#endif
    return s;
}

struct DepSpec {
    std::string name, git, rev, path;
    std::string requiredBy;  // "" for the project itself
    bool sameRepo(const DepSpec& o) const { return sameRepoKey(git) == sameRepoKey(o.git) && path == o.path; }
    bool sameSource(const DepSpec& o) const { return sameRepo(o) && rev == o.rev; }
    std::string describe() const {
        if (!path.empty()) return "path " + path;
        return git + (rev.empty() ? "" : " @ " + rev);
    }
};

struct LockEntry {
    std::string git, rev, path, commit;
};

bool writeText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << text;
    return (bool)f;
}

bool validName(const std::string& n) {
    if (n.empty()) return false;
    for (char c : n)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return !std::isdigit((unsigned char)n[0]);
}

// "https://host/user/my-lib.git" -> "my_lib"
std::string nameFromSource(std::string src) {
    while (!src.empty() && (src.back() == '/' || src.back() == '\\')) src.pop_back();
    size_t slash = src.find_last_of("/\\:");
    std::string base = slash == std::string::npos ? src : src.substr(slash + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".git") == 0) base.resize(base.size() - 4);
    if (base.rfind("ixo-", 0) == 0 || base.rfind("ixo_", 0) == 0) base = base.substr(4);
    std::string out;
    for (char c : base) out += std::isalnum((unsigned char)c) ? c : '_';
    if (!out.empty() && std::isdigit((unsigned char)out[0])) out = "_" + out;
    return out;
}

bool looksLikeGit(const std::string& s) {
    return s.find("://") != std::string::npos || s.rfind("git@", 0) == 0 ||
           (s.size() > 4 && s.compare(s.size() - 4, 4, ".git") == 0);
}

std::vector<DepSpec> depsOf(const TomlDoc& doc, const std::string& requiredBy, std::string& err) {
    std::vector<DepSpec> out;
    auto it = doc.sections.find("dependencies");
    if (it == doc.sections.end()) return out;
    for (auto& [name, v] : it->second) {
        DepSpec d;
        d.name = name;
        d.requiredBy = requiredBy;
        if (v.k == TomlValue::Table) {
            d.git = v.str("git");
            d.rev = v.str("rev");
            d.path = v.str("path");
        } else if (v.k == TomlValue::Str) {
            (looksLikeGit(v.s) ? d.git : d.path) = v.s;
        }
        if (d.git.empty() == d.path.empty()) {
            err = "dependency '" + name + "' needs either git = \"...\" or path = \"...\"";
            return {};
        }
        out.push_back(d);
    }
    return out;
}

// Edits one line of [dependencies] in ixo.toml text, keeping everything else
// as it is. An empty `line` removes the entry.
std::string setDependencyLine(const std::string& text, const std::string& name, const std::string& line) {
    std::vector<std::string> lines;
    size_t s = 0;
    while (s <= text.size()) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) {
            if (s < text.size()) lines.push_back(text.substr(s));
            break;
        }
        lines.push_back(text.substr(s, e - s));
        s = e + 1;
    }
    auto trimmed = [](std::string x) {
        while (!x.empty() && (x.back() == '\r' || x.back() == ' ' || x.back() == '\t')) x.pop_back();
        size_t a = x.find_first_not_of(" \t");
        return a == std::string::npos ? std::string() : x.substr(a);
    };
    int sec = -1, end = (int)lines.size();
    for (int i = 0; i < (int)lines.size(); i++) {
        std::string t = trimmed(lines[i]);
        if (!t.empty() && t[0] == '[') {
            if (sec >= 0) {
                end = i;
                break;
            }
            if (t == "[dependencies]") sec = i;
        }
    }
    if (sec < 0) {
        if (line.empty()) return text;
        std::string out = text;
        if (!out.empty() && out.back() != '\n') out += "\n";
        if (!out.empty()) out += "\n";
        return out + "[dependencies]\n" + line + "\n";
    }
    int found = -1;
    for (int i = sec + 1; i < end; i++) {
        std::string t = trimmed(lines[i]);
        if (t.empty() || t[0] == '#') continue;
        std::string key = t.substr(0, t.find_first_of(" =\t"));
        if (key.size() >= 2 && (key[0] == '"' || key[0] == '\'')) key = key.substr(1, key.size() - 2);
        if (key == name) found = i;
    }
    if (found >= 0) {
        if (line.empty())
            lines.erase(lines.begin() + found);
        else
            lines[found] = line;
    } else if (!line.empty()) {
        int at = end;
        while (at > sec + 1 && trimmed(lines[at - 1]).empty()) at--;  // before trailing blank lines
        lines.insert(lines.begin() + at, line);
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) out += lines[i] + "\n";
    return out;
}

std::string depLine(const DepSpec& d) {
    std::string v = "{ ";
    if (!d.path.empty()) {
        v += "path = " + tomlQuote(d.path);
    } else {
        v += "git = " + tomlQuote(d.git);
        if (!d.rev.empty()) v += ", rev = " + tomlQuote(d.rev);
    }
    return d.name + " = " + v + " }";
}

struct Git {
    std::string err;
    bool missing = false;
    // Runs git; on failure keeps a one-line reason in `err`.
    bool run(const std::vector<std::string>& args, std::string* out = nullptr, const std::string& cwd = "") {
        std::vector<std::string> argv{"git"};
        argv.insert(argv.end(), args.begin(), args.end());
        RunResult r = runProcess(argv, "", cwd);
        if (!r.started) {
            missing = true;
            err = "git isn't installed or isn't on PATH (" + r.error + ")";
            return false;
        }
        if (r.code != 0) {
            std::string m = r.err.empty() ? r.out : r.err;
            while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
            err = m.empty() ? "git " + args[0] + " failed" : m;
            return false;
        }
        if (out) {
            *out = r.out;
            while (!out->empty() && (out->back() == '\n' || out->back() == '\r' || out->back() == ' ')) out->pop_back();
        }
        return true;
    }
    // The commit `rev` names ("" = the remote's default branch), after a fetch.
    bool resolve(const std::string& dir, const std::string& rev, std::string& commit) {
        std::vector<std::string> tries;
        if (rev.empty()) {
            tries = {"origin/HEAD", "HEAD"};
        } else {
            tries = {"origin/" + rev, rev};
        }
        for (auto& t : tries)
            if (run({"-C", dir, "rev-parse", "--verify", "--quiet", t + "^{commit}"}, &commit)) return true;
        err = "no branch, tag or commit named '" + (rev.empty() ? "HEAD" : rev) + "'";
        return false;
    }
};

// remove_all, but git's object files are read-only (Windows refuses to delete those)
void removeTree(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec))) return;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
    fs::remove_all(p, ec);
}

bool copyTree(const fs::path& from, const fs::path& to, std::string& err) {
    std::error_code ec;
    removeTree(to);
    fs::create_directories(to, ec);
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::string leaf = S(it->path().filename());
        if (it->is_directory() && (leaf == ".git" || leaf == "ixo_modules" || leaf == "build")) {
            it.disable_recursion_pending();
            continue;
        }
        fs::path rel = fs::relative(it->path(), from, ec);
        if (it->is_directory())
            fs::create_directories(to / rel, ec);
        else if (it->is_regular_file())
            fs::copy_file(it->path(), to / rel, fs::copy_options::overwrite_existing, ec);
        if (ec) break;
    }
    if (ec) err = ec.message();
    return !ec;
}

struct Project {
    fs::path root;
    std::string tomlText;
    TomlDoc toml;
    std::map<std::string, LockEntry> lock;

    fs::path modules() const { return root / "ixo_modules"; }

    bool load(std::string& err) {
        std::string r = findProjectRoot(".");
        if (r.empty()) {
            err = "no ixo.toml here or in a parent folder (run `ixo init` first)";
            return false;
        }
        root = P(r);
        bool ok = false;
        tomlText = readFile(S(root / "ixo.toml"), &ok);
        std::string perr;
        if (!ok || !parseToml(tomlText, toml, &perr)) {
            err = "ixo.toml: " + (ok ? perr : "can't read it");
            return false;
        }
        lock.clear();
        std::string lt = readFile(S(root / "ixo.lock"), &ok);
        TomlDoc ld;
        if (ok && parseToml(lt, ld, nullptr)) {
            for (auto& [sec, kv] : ld.sections) {
                if (sec.rfind("packages.", 0) != 0) continue;
                LockEntry e;
                e.git = ld.str(sec, "git");
                e.rev = ld.str(sec, "rev");
                e.path = ld.str(sec, "path");
                e.commit = ld.str(sec, "commit");
                lock[sec.substr(9)] = e;
            }
        }
        return true;
    }

    bool saveToml(std::string& err) {
        if (!writeText(root / "ixo.toml", tomlText)) {
            err = "can't write ixo.toml";
            return false;
        }
        return true;
    }

    bool saveLock(std::string& err) {
        std::string out = "# Written by `ixo install`: the exact versions of every package. Commit this file.\n";
        for (auto& [name, e] : lock) {
            out += "\n[packages." + name + "]\n";
            if (!e.path.empty()) {
                out += "path = " + tomlQuote(e.path) + "\n";
            } else {
                out += "git = " + tomlQuote(e.git) + "\n";
                if (!e.rev.empty()) out += "rev = " + tomlQuote(e.rev) + "\n";
                out += "commit = " + tomlQuote(e.commit) + "\n";
            }
        }
        if (!writeText(root / "ixo.lock", out)) {
            err = "can't write ixo.lock";
            return false;
        }
        return true;
    }

    // Puts one package in ixo_modules/<name>. `update`: ignore the locked commit.
    bool fetch(const DepSpec& d, bool update, Git& git, LockEntry& out, std::string& err) {
        fs::path dir = modules() / P(d.name);
        std::error_code ec;
        fs::create_directories(modules(), ec);
        out = LockEntry{d.git, d.rev, d.path, ""};
        if (!d.path.empty()) {
            fs::path src = P(d.path);
            if (src.is_relative()) src = root / src;
            if (!fs::is_directory(src, ec)) {
                err = "'" + d.name + "': no folder at " + d.path;
                return false;
            }
            if (!copyTree(src, dir, err)) {
                err = "'" + d.name + "': " + err;
                return false;
            }
            std::printf("  %-14s %s\n", d.name.c_str(), d.path.c_str());
            return true;
        }
        auto lk = lock.find(d.name);
        std::string want;
        if (!update && lk != lock.end() && lk->second.git == d.git && lk->second.rev == d.rev) want = lk->second.commit;
        std::string sdir = S(dir);
        bool have = fs::is_directory(dir / ".git", ec);
        if (have) {
            std::string url;
            if (!git.run({"-C", sdir, "remote", "get-url", "origin"}, &url) || sameRepoKey(url) != sameRepoKey(d.git)) {
                removeTree(dir);
                have = false;
            }
        } else if (fs::exists(dir, ec)) {
            removeTree(dir);
        }
        if (!have) {
            std::printf("  fetching %s\n", d.git.c_str());
            std::fflush(stdout);
            if (!git.run({"clone", "--quiet", d.git, sdir})) {
                err = "'" + d.name + "': " + git.err;
                return false;
            }
        }
        std::string commit;
        if (!want.empty()) {
            bool present = git.run({"-C", sdir, "cat-file", "-e", want + "^{commit}"});
            if (!present && !git.run({"-C", sdir, "fetch", "--quiet", "--tags", "origin"})) {
                err = "'" + d.name + "': " + git.err;
                return false;
            }
            commit = want;
        } else {
            if (have && !git.run({"-C", sdir, "fetch", "--quiet", "--tags", "origin"})) {
                err = "'" + d.name + "': " + git.err;
                return false;
            }
            if (have) git.run({"-C", sdir, "remote", "set-head", "origin", "--auto"});
            if (!git.resolve(sdir, d.rev, commit)) {
                err = "'" + d.name + "': " + git.err;
                return false;
            }
        }
        if (!git.run({"-C", sdir, "checkout", "--quiet", "--detach", commit})) {
            err = "'" + d.name + "': " + git.err;
            return false;
        }
        out.commit = commit;
        std::printf("  %-14s %s %s\n", d.name.c_str(), commit.substr(0, 10).c_str(),
                    d.rev.empty() ? "" : ("(" + d.rev + ")").c_str());
        return true;
    }

    // Installs the project's dependencies and theirs. `updateAll` / `updateOnly`:
    // which packages may move to newer commits.
    bool install(bool updateAll, const std::set<std::string>& updateOnly, std::string& err) {
        std::string derr;
        std::vector<DepSpec> top = depsOf(toml, "", derr);
        if (!derr.empty()) {
            err = "ixo.toml: " + derr;
            return false;
        }
        std::map<std::string, DepSpec> chosen;
        std::map<std::string, LockEntry> newLock;
        std::deque<DepSpec> queue(top.begin(), top.end());
        Git git;
        while (!queue.empty()) {
            DepSpec d = queue.front();
            queue.pop_front();
            if (!validName(d.name)) {
                err = "'" + d.name + "' isn't a valid package name (letters, digits and _; it's used in `use " +
                      d.name + "`)";
                return false;
            }
            auto c = chosen.find(d.name);
            if (c != chosen.end()) {
                if (!c->second.sameSource(d)) {
                    std::string who = c->second.requiredBy.empty() ? "the project" : c->second.requiredBy;
                    if (!c->second.sameRepo(d)) {
                        err = "two different packages are called '" + d.name + "':\n    " + c->second.describe() +
                              " (needed by " + who + ")\n    " + d.describe() + " (needed by " + d.requiredBy + ")";
                        return false;
                    }
                    std::fprintf(stderr, "  note: %s wants %s %s; keeping %s from %s\n", d.requiredBy.c_str(),
                                 d.name.c_str(), d.rev.empty() ? "(latest)" : d.rev.c_str(),
                                 c->second.rev.empty() ? "(latest)" : c->second.rev.c_str(), who.c_str());
                }
                continue;
            }
            chosen[d.name] = d;
            LockEntry e;
            bool upd = updateAll || updateOnly.count(d.name);
            if (!fetch(d, upd, git, e, err)) {
                if (git.missing) err = git.err;
                return false;
            }
            newLock[d.name] = e;
            // its own dependencies
            bool ok = false;
            std::string text = readFile(S(modules() / P(d.name) / "ixo.toml"), &ok);
            TomlDoc sub;
            std::string perr;
            if (ok && parseToml(text, sub, &perr)) {
                std::vector<DepSpec> more = depsOf(sub, d.name, derr);
                if (!derr.empty()) {
                    err = d.name + "/ixo.toml: " + derr;
                    return false;
                }
                for (auto& m : more) {
                    if (!m.path.empty() && P(m.path).is_relative()) {
                        // relative to that package's own source folder
                        if (!d.path.empty()) {
                            fs::path base = P(d.path);
                            m.path = S((base / P(m.path)).lexically_normal());
                        } else {
                            err = d.name + " depends on '" + m.name + "' by a relative path, which only works locally";
                            return false;
                        }
                    }
                    queue.push_back(m);
                }
            } else if (ok) {
                err = d.name + "/ixo.toml: " + perr;
                return false;
            }
        }
        // remove packages nothing needs any more (only ones we installed)
        for (auto& [name, e] : lock) {
            if (newLock.count(name)) continue;
            removeTree(modules() / P(name));
            std::printf("  removed %s\n", name.c_str());
        }
        lock = newLock;
        if (!saveLock(err)) return false;
        std::printf("%zu package%s installed\n", lock.size(), lock.size() == 1 ? "" : "s");
        return true;
    }
};

void ensureGitignore(const fs::path& root) {
    bool ok = false;
    std::string gi = readFile(S(root / ".gitignore"), &ok);
    if (gi.find("ixo_modules") != std::string::npos) return;
    if (!gi.empty() && gi.back() != '\n') gi += "\n";
    writeText(root / ".gitignore", gi + "ixo_modules/\n");
}

int fail(const std::string& msg) {
    std::fprintf(stderr, "ixo: %s\n", msg.c_str());
    return 1;
}

}  // namespace

int runInit(const std::vector<std::string>& args) {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    if (fs::exists(dir / "ixo.toml", ec)) return fail("ixo.toml already exists here");
    std::string name = args.empty() ? nameFromSource(S(dir.filename())) : args[0];
    if (!validName(name)) return fail("'" + name + "' isn't a valid package name (letters, digits and _)");
    std::string main = fs::exists(dir / "main.ixo", ec) ? "main.ixo" : fs::exists(dir / "lib.ixo", ec) ? "lib.ixo" : "main.ixo";
    if (!writeText(dir / "ixo.toml", "[package]\nname = \"" + name + "\"\nversion = \"0.1.0\"\nmain = \"" + main +
                                         "\"\n\n[dependencies]\n"))
        return fail("can't write ixo.toml");
    ensureGitignore(dir);
    std::printf("created ixo.toml for '%s'\n", name.c_str());
    return 0;
}

int runAdd(const std::vector<std::string>& args) {
    DepSpec d;
    std::string source;
    for (size_t i = 0; i < args.size(); i++) {
        if ((args[i] == "--rev" || args[i] == "--name") && i + 1 < args.size()) {
            (args[i] == "--rev" ? d.rev : d.name) = args[i + 1];
            i++;
        } else if (args[i] == "--path" && i + 1 < args.size()) {
            d.path = args[++i];
        } else if (args[i].rfind("--", 0) == 0) {
            return fail("unknown option " + args[i]);
        } else if (source.empty()) {
            source = args[i];
        } else if (d.name.empty()) {
            d.name = args[i];
        } else {
            return fail("too many arguments");
        }
    }
    if (source.empty() && d.path.empty())
        return fail("usage: ixo add <git-url | folder> [name] [--rev branch|tag|commit]");
    Project p;
    std::string err;
    if (!p.load(err)) return fail(err);
    if (d.path.empty()) {
        std::error_code ec;
        bool isDir = !looksLikeGit(source) && fs::is_directory(P(source), ec);
        if (isDir && !fs::exists(P(source) / ".git", ec)) {
            d.path = source;
        } else {
            d.git = source;
            if (isDir) d.git = S(fs::absolute(P(source), ec));  // a local repository
        }
    }
    if (!d.path.empty() && d.path.rfind(".", 0) != 0 && P(d.path).is_relative()) d.path = "./" + d.path;
    if (!d.path.empty()) {
        // store it relative to the project root, with forward slashes
        std::error_code ec;
        fs::path rel = fs::relative(fs::absolute(P(d.path), ec), p.root, ec);
        if (!ec && !rel.empty()) d.path = S(rel);
        std::replace(d.path.begin(), d.path.end(), '\\', '/');
    }
    if (d.name.empty()) {
        // the package's own name, when its ixo.toml says one
        if (!d.path.empty()) {
            bool ok = false;
            TomlDoc sub;
            std::string text = readFile(S(p.root / P(d.path) / "ixo.toml"), &ok);
            if (ok && parseToml(text, sub, nullptr)) d.name = sub.str("package", "name");
        }
        if (d.name.empty()) d.name = nameFromSource(d.path.empty() ? d.git : d.path);
    }
    if (!validName(d.name)) return fail("'" + d.name + "' isn't a valid package name; give one: ixo add <source> <name>");
    std::string oldText = p.tomlText;
    p.tomlText = setDependencyLine(p.tomlText, d.name, depLine(d));
    p.toml = TomlDoc{};
    std::string perr;
    if (!parseToml(p.tomlText, p.toml, &perr)) return fail("ixo.toml: " + perr);
    if (!p.saveToml(err)) return fail(err);
    std::printf("added %s (%s)\n", d.name.c_str(), d.describe().c_str());
    ensureGitignore(p.root);
    if (!p.install(false, {d.name}, err)) {
        p.tomlText = oldText;  // leave ixo.toml as it was
        std::string e2;
        p.saveToml(e2);
        return fail(err);
    }
    std::printf("\nuse it with:  use %s\n", d.name.c_str());
    return 0;
}

int runInstall(const std::vector<std::string>& args) {
    Project p;
    std::string err;
    if (!p.load(err)) return fail(err);
    if (!p.install(false, {}, err)) return fail(err);
    return 0;
}

int runUpdate(const std::vector<std::string>& args) {
    Project p;
    std::string err;
    if (!p.load(err)) return fail(err);
    std::set<std::string> only(args.begin(), args.end());
    for (auto& n : only)
        if (!p.toml.get("dependencies", n) && !p.lock.count(n)) return fail("no package called '" + n + "'");
    if (!p.install(only.empty(), only, err)) return fail(err);
    return 0;
}

int runRemove(const std::vector<std::string>& args) {
    if (args.empty()) return fail("usage: ixo remove <name>...");
    Project p;
    std::string err;
    if (!p.load(err)) return fail(err);
    for (auto& n : args) {
        if (!p.toml.get("dependencies", n)) return fail("ixo.toml has no dependency called '" + n + "'");
        p.tomlText = setDependencyLine(p.tomlText, n, "");
    }
    p.toml = TomlDoc{};
    std::string perr;
    if (!parseToml(p.tomlText, p.toml, &perr)) return fail("ixo.toml: " + perr);
    if (!p.saveToml(err)) return fail(err);
    for (auto& n : args) std::printf("removed %s from ixo.toml\n", n.c_str());
    if (!p.install(false, {}, err)) return fail(err);
    return 0;
}

}  // namespace ixo
