// REPL, @test runner and code formatter.
#include "tools.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <thread>
#include <unordered_set>

#include "builtins.hpp"
#include "common.hpp"
#include "checker.hpp"
#include "compiler.hpp"
#include "json.hpp"
#include "lineedit.hpp"
#include "parser.hpp"
#include "process.hpp"
#include "program.hpp"
#include "resolver.hpp"

namespace ixo {

namespace fs = std::filesystem;

// ================================================================== REPL

static bool needsMoreInput(const std::string& src) {
    // A bare `fn name(...)` / `class Name` header waits for its indented body.
    size_t lastStart = src.find_last_of('\n', src.size() >= 2 ? src.size() - 2 : 0);
    std::string last = src.substr(lastStart == std::string::npos ? 0 : lastStart + 1);
    last.erase(0, last.find_first_not_of(" \t"));
    while (!last.empty() && (last.back() == '\n' || last.back() == ' ')) last.pop_back();
    for (const char* header : {"fn ", "class ", "fork fn ", "par fn ", "interface ", "enum "})
        if (last.rfind(header, 0) == 0 && last.find('=') == std::string::npos) return true;
    Diagnostics d;
    Lexer lx(src, "<repl>", d);
    auto toks = lx.tokenize();
    // unclosed brackets or strings
    int depth = 0;
    for (auto& t : toks) {
        if (t.kind == T::LParen || t.kind == T::LBracket || t.kind == T::LBrace) depth++;
        if (t.kind == T::RParen || t.kind == T::RBracket || t.kind == T::RBrace) depth--;
    }
    if (depth > 0) return true;
    for (auto& diag : d.list)
        if (diag.message.find("unterminated string") != std::string::npos) return true;
    Parser p(std::move(toks), "<repl>", d);
    p.parseProgram();
    for (auto& diag : d.list)
        if (diag.message.find("indented block") != std::string::npos ||
            diag.message.find("found end of file") != std::string::npos)
            return true;
    return false;
}

// Runs one piece of source in the REPL's module. `typeOnly`: print the static
// type of the (single) expression instead of running it.
static bool runReplChunk(Program& prog, ModuleUnit* mod, const std::string& src, bool typeOnly) {
    mod->source = src;
    prog.sources[mod->path] = src;
    Diagnostics diags;
    Lexer lexer(src, mod->path, diags);
    Parser parser(lexer.tokenize(), mod->path, diags);
    auto chunk = parser.parseProgram();
    if (!diags.hasErrors()) {
        Resolver resolver(*mod, diags, prog);
        resolver.resolveModule(chunk.get());
    }
    if (!diags.hasErrors()) {
        Checker checker(prog, *mod, diags);
        checker.check(chunk.get());
    }
    if (diags.hasErrors()) {
        prog.printDiagnostics(diags);
        mod->replChunks.push_back(std::move(chunk));
        return false;
    }
    if (typeOnly) {
        if (!chunk->stmts.empty() && chunk->stmts[0]->kind == NK::ExprStmt) {
            Expr* e = static_cast<ExprStmt*>(chunk->stmts[0].get())->expr.get();
            std::printf("%s\n", typeToString(e->type).c_str());
        }
        mod->replChunks.push_back(std::move(chunk));
        return true;
    }
    Compiler compiler(prog.vm, *mod, diags);
    ObjFunction* fn = compiler.compileModule(chunk.get(), false, true);
    mod->replChunks.push_back(std::move(chunk));
    if (diags.hasErrors()) {
        prog.printDiagnostics(diags);
        return false;
    }
    mod->rt->script = prog.vm.alloc<ObjClosure>(fn);
    prog.vm.extraRoots.push_back(Value::object(mod->rt->script));  // par workers may still run its code
    if (!prog.vm.runModule(mod->rt)) return false;
    Value r = prog.vm.lastResult;
    if (!r.isNil()) writeOut(prog.vm.repr(r) + "\n");
    return true;
}

static const char* kReplWords[] = {"fn", "class", "interface", "enum", "if", "elif", "else", "while", "for", "in",
                                   "not", "and", "or", "is", "return", "match", "use", "var", "with", "fork", "par",
                                   "defer", "true", "false", "nil", "break", "continue", "retry", "self", "as", "todo"};

// Names that complete `before` (the text left of the cursor).
static std::pair<size_t, std::vector<std::string>> replComplete(Program& prog, ModuleUnit* mod, const std::string& before) {
    VM& vm = prog.vm;
    auto wordChar = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    size_t start = before.size();
    while (start > 0 && wordChar(before[start - 1])) start--;
    std::string prefix = before.substr(start);
    std::vector<std::string> names;
    auto globalValue = [&](const std::string& n, Value& v) {
        for (size_t i = 0; i < mod->rt->globalNames.size() && i < mod->rt->globals.size(); i++)
            if (mod->rt->globalNames[i] == n && !mod->rt->globals[i].isUndef()) {
                v = mod->rt->globals[i];
                return true;
            }
        int idx = vm.builtinIndex(n);
        if (idx >= 0) {
            v = vm.builtins[idx].value;
            return true;
        }
        return false;
    };
    if (start > 0 && before[start - 1] == '.') {
        size_t e = start - 1, b = e;
        while (b > 0 && wordChar(before[b - 1])) b--;
        Value v;
        std::string recv = before.substr(b, e - b);
        bool found = false;
        if (!recv.empty() && std::isdigit((unsigned char)recv[0])) {
            v = recv.find_first_not_of("0123456789_") == std::string::npos ? Value::integer(0) : Value::number(0);
            found = true;
        } else if (recv.empty() && e > 0 && (before[e - 1] == '"' || before[e - 1] == '\'')) {
            v = vm.vstr("");
            found = true;
        } else if (recv.empty() && e > 0 && before[e - 1] == ']') {
            v = Value::object(vm.alloc<ObjList>());
            found = true;
        } else if (recv.empty() && e > 0 && before[e - 1] == '}') {
            v = Value::object(vm.alloc<ObjMap>());
            found = true;
        } else if (!recv.empty()) {
            found = globalValue(recv, v);
        }
        if (!found) return {start, {}};
        if (isObjType(v, OT::Instance)) {
            auto* inst = as<ObjInstance>(v);
            for (auto& f : inst->cls->fields) names.push_back(f);
            for (auto& [sym, m] : inst->cls->methods) names.push_back(vm.symName(sym));
        } else if (isObjType(v, OT::Namespace)) {
            for (auto& [k, m] : as<ObjNamespace>(v)->members) names.push_back(k);
        } else {
            Tag tag = vm.tagOf(v);
            if (tag != Tag::COUNT)
                for (auto& [sym, m] : vm.methods[(int)tag]) names.push_back(vm.symName(sym));
        }
    } else if (start == 0 || before.find_first_not_of(" ") == start) {
        if (!before.empty() && before[0] == ':') {
            for (const char* c : {":help", ":type ", ":load ", ":reload", ":quit"}) names.push_back(c);
            start = 0;
            prefix = before;
        }
    }
    if (names.empty() && (start == 0 || before[start - 1] != '.')) {
        for (const char* k : kReplWords) names.push_back(k);
        for (auto& b : vm.builtins) names.push_back(b.name);
        for (auto& g : mod->rt->globalNames) names.push_back(g);
    }
    std::vector<std::string> out;
    for (auto& n : names)
        if (!n.empty() && n[0] != '$' && n.rfind(prefix, 0) == 0 && (n.find("__") == std::string::npos)) out.push_back(n);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return {start, out};
}

int runRepl() {
    Program prog;
    prog.units.push_back(std::make_unique<ModuleUnit>());
    ModuleUnit* mod = prog.units.back().get();
    mod->name = "<repl>";
    mod->path = "<repl>";
    mod->rt = prog.vm.newModule("<repl>", "<repl>");
    LineEditor ed("");
    ed.completer = [&](const std::string& before) { return replComplete(prog, mod, before); };
    std::printf("Ixo REPL. Type :help for help, :quit to exit.\n");
    std::string pending, lastLoaded;
    auto load = [&](const std::string& file) {
        bool ok = false;
        std::string text = readFile(file, &ok);
        if (!ok) {
            std::printf("can't read '%s'\n", file.c_str());
            return;
        }
        lastLoaded = file;
        // the file's own imports resolve next to it
        std::string saved = mod->path;
        mod->path = canonicalPath(file);
        runReplChunk(prog, mod, text, false);
        mod->path = saved;
        flushOut();
    };
    while (true) {
        flushOut();
        std::string line;
        LineEditor::Result r = ed.read(pending.empty() ? ">>> " : "... ", line);
        if (r == LineEditor::End) break;
        if (r == LineEditor::Cancelled) {
            pending.clear();
            continue;
        }
        if (pending.empty()) {
            std::string cmd = line;
            cmd.erase(0, cmd.find_first_not_of(" \t"));
            while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '\t')) cmd.pop_back();
            if (cmd == ":quit" || cmd == ":q" || cmd == "exit" || cmd == "quit") break;
            if (cmd == ":help") {
                std::printf(
                    "Type Ixo code and press Enter. Blocks continue until an empty line.\n"
                    "  :type <expr>   show the static type of an expression\n"
                    "  :load <file>   run a file here (its functions and variables stay defined)\n"
                    "  :reload        run the last loaded file again\n"
                    "  :quit          leave (or Ctrl+D)\n"
                    "Keys: arrows move and browse history, Tab completes, Ctrl+C clears the input.\n");
                continue;
            }
            if (cmd.rfind(":load ", 0) == 0) {
                ed.addHistory(cmd);
                std::string file = cmd.substr(6);
                file.erase(0, file.find_first_not_of(" \t\""));
                while (!file.empty() && (file.back() == '"' || file.back() == ' ')) file.pop_back();
                load(file);
                continue;
            }
            if (cmd == ":reload") {
                ed.addHistory(cmd);
                if (lastLoaded.empty())
                    std::printf("nothing loaded yet (use :load <file>)\n");
                else
                    load(lastLoaded);
                continue;
            }
            if (cmd.empty()) continue;
        }
        bool continuing = !pending.empty();
        pending += line + "\n";
        bool isType = pending.rfind(":type ", 0) == 0;
        if (!isType) {
            if (continuing) {
                if (!line.empty()) continue;  // a block continues until an empty line
            } else if (needsMoreInput(pending)) {
                continue;
            }
        }
        std::string entry = pending;
        while (!entry.empty() && entry.back() == '\n') entry.pop_back();
        ed.addHistory(entry);
        std::string src = isType ? pending.substr(6) : pending;
        pending.clear();
        runReplChunk(prog, mod, src, isType);
    }
    flushOut();
    if (ParRuntime* rt = ParRuntime::get()) rt->shutdown();
    return 0;
}

// ================================================================== @test runner

static void collectFiles(const std::string& arg, std::vector<std::string>& out) {
    std::error_code ec;
    fs::path p(std::u8string(arg.begin(), arg.end()));
    if (fs::is_directory(p, ec)) {
        for (auto& e : fs::recursive_directory_iterator(p, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".ixo") continue;
            auto u = e.path().u8string();
            out.push_back(std::string(u.begin(), u.end()));
        }
    } else {
        out.push_back(arg);
    }
}

static void testEvent(const Json& j) {
    flushOut();
    std::printf("@@ixo %s\n", j.dump().c_str());
    std::fflush(stdout);
}

// ixo test [--filter text] [--json] [files or dirs]
int runTests(const std::vector<std::string>& args) {
    std::vector<std::string> files;
    std::vector<std::string> targets;
    std::string filter;
    bool json = false;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--filter" && i + 1 < args.size()) {
            filter = args[++i];
        } else if (args[i].rfind("--filter=", 0) == 0) {
            filter = args[i].substr(9);
        } else {
            targets.push_back(args[i]);
        }
    }
    if (targets.empty()) targets.push_back(fs::exists("tests") ? "tests" : ".");
    for (auto& t : targets) collectFiles(t, files);
    std::sort(files.begin(), files.end());
    int passed = 0, failed = 0, filesWithTests = 0;
    for (auto& f : files) {
        bool ok = false;
        std::string src = readFile(f, &ok);
        if (!ok || src.find("@test") == std::string::npos) continue;
        Program prog;
        Diagnostics diags;
        CompileOptions o = prog.opts;
        o.isEntry = false;
        ModuleUnit* m = prog.loadFile(f, diags, o);
        auto fileFailed = [&](const std::string& why) {
            failed++;
            if (json) {
                Json j = Json::object();
                j.set("event", "file");
                j.set("file", canonicalPath(f));
                j.set("status", "failed");
                j.set("message", why);
                testEvent(j);
            } else {
                std::printf("FAIL %s (%s)\n", f.c_str(), why.c_str());
            }
        };
        if (!m || diags.hasErrors()) {
            prog.printDiagnostics(diags);
            fileFailed("does not compile");
            continue;
        }
        std::vector<FnDecl*> tests;
        for (FnDecl* t : m->tests)
            if (t->decl && (filter.empty() || t->name.find(filter) != std::string::npos)) tests.push_back(t);
        if (tests.empty()) continue;
        filesWithTests++;
        if (!prog.run(m)) {
            fileFailed("top-level code panicked: " + prog.vm.lastPanic);
            continue;
        }
        for (FnDecl* t : tests) {
            Value fn = m->rt->globals[t->decl->globalIndex];
            auto t0 = std::chrono::steady_clock::now();
            std::string msg = prog.vm.runTest(fn);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            flushOut();
            if (msg.empty())
                passed++;
            else
                failed++;
            if (json) {
                Json j = Json::object();
                j.set("event", "test");
                j.set("file", canonicalPath(f));
                j.set("name", t->name);
                j.set("line", t->pos.line);
                j.set("status", msg.empty() ? "passed" : "failed");
                j.set("message", msg);
                j.set("ms", ms);
                testEvent(j);
            } else if (msg.empty()) {
                std::printf("  ok   %s: %s\n", f.c_str(), t->name.c_str());
            } else {
                std::string indented;
                for (char ch : msg) indented += ch == '\n' ? std::string("\n       ") : std::string(1, ch);
                std::printf("  FAIL %s: %s\n       %s\n", f.c_str(), t->name.c_str(), indented.c_str());
            }
        }
    }
    if (json) {
        Json j = Json::object();
        j.set("event", "summary");
        j.set("passed", passed);
        j.set("failed", failed);
        testEvent(j);
    }
    std::printf("\n%d passed, %d failed (%d file%s)\n", passed, failed, filesWithTests, filesWithTests == 1 ? "" : "s");
    std::fflush(stdout);
    if (ParRuntime::get()) hardExit(failed ? 1 : 0);  // par workers may still hold tasks of failed tests
    return failed ? 1 : 0;
}

// ================================================================== watch mode

// The .ixo files under the targets and every module they import.
static std::vector<std::string> watchedFiles(const std::vector<std::string>& targets) {
    std::vector<std::string> files;
    for (auto& t : targets) collectFiles(t, files);
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (auto& f : files) {
        if (seen.insert(canonicalPath(f)).second) out.push_back(f);
        Program prog;
        prog.opts.compile = false;
        prog.opts.typeCheck = false;
        Diagnostics diags;
        prog.loadFile(f, diags, prog.opts);
        for (auto& [path, u] : prog.byPath)
            if (u && !u->path.empty() && u->path[0] != '<' && seen.insert(canonicalPath(u->path)).second)
                out.push_back(u->path);
    }
    return out;
}

int runWatch(const std::vector<std::string>& childArgs, const std::vector<std::string>& targets) {
    using Stamps = std::map<std::string, fs::file_time_type>;
    auto stampsOf = [](const std::vector<std::string>& files) {
        Stamps st;
        for (auto& f : files) {
            std::error_code ec;
            fs::path p(std::u8string(f.begin(), f.end()));
            st[f] = fs::last_write_time(p, ec);
        }
        return st;
    };
    std::vector<std::string> argv{selfExecutable()};
    argv.insert(argv.end(), childArgs.begin(), childArgs.end());
    while (true) {
        std::vector<std::string> files = watchedFiles(targets);
        Stamps stamps = stampsOf(files);
        std::fprintf(stderr, "\x1b[2m[watch] ixo");
        for (auto& a : childArgs) std::fprintf(stderr, " %s", a.c_str());
        std::fprintf(stderr, "  (%zu file%s watched)\x1b[0m\n", files.size(), files.size() == 1 ? "" : "s");
        std::fflush(stderr);
        std::string err;
        auto child = Process::spawn(argv, ProcessOptions{}, &err);
        if (!child) {
            std::fprintf(stderr, "ixo: %s\n", err.c_str());
            return 1;
        }
        bool reported = false;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            if (!reported && !child->running()) {
                int code = child->wait();
                std::fprintf(stderr, "\x1b[2m[watch] exited with code %d; waiting for changes (Ctrl+C to stop)\x1b[0m\n",
                             code);
                std::fflush(stderr);
                reported = true;
            }
            Stamps now = stampsOf(files);
            // a file created in a watched directory also counts
            bool grew = false;
            if (reported) {
                std::vector<std::string> fresh;
                for (auto& t : targets) collectFiles(t, fresh);
                for (auto& f : fresh)
                    if (!stamps.count(f)) grew = true;
            }
            if (now != stamps || grew) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // editors save in several steps
        if (child->running()) child->kill();
        std::fprintf(stderr, "\n\x1b[2m[watch] change detected, restarting\x1b[0m\n");
    }
}

// ================================================================== ixo new

int runNewProject(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "usage: ixo new <name>\n");
        return 2;
    }
    std::string name = args[0];
    fs::path root(std::u8string(name.begin(), name.end()));
    std::error_code ec;
    if (fs::exists(root, ec) && !fs::is_empty(root, ec)) {
        std::fprintf(stderr, "ixo new: '%s' already exists and isn't empty\n", name.c_str());
        return 1;
    }
    std::string pkg = root.filename().string();
    auto write = [&](const std::string& rel, const std::string& text) {
        fs::path p = root / fs::path(std::u8string(rel.begin(), rel.end()));
        fs::create_directories(p.parent_path(), ec);
        FILE* fo = std::fopen(p.string().c_str(), "wb");
        if (!fo) return false;
        std::fwrite(text.data(), 1, text.size(), fo);
        std::fclose(fo);
        return true;
    };
    bool ok = write("ixo.toml", "[package]\nname = \"" + pkg + "\"\nversion = \"0.1.0\"\nmain = \"main.ixo\"\n\n[dependencies]\n") &&
              write("main.ixo", "use lib.greeting\n\nfn main\n    print(greet(\"world\"))\n") &&
              write("lib/greeting.ixo",
                    "fn greet*(name: str) -> str\n    \"\"\"A friendly greeting for `name`.\"\"\"\n    f\"Hello, {name}!\"\n") &&
              write("tests/greeting_test.ixo",
                    "use lib.greeting\n\n@test\nfn greets_by_name\n    assert_eq(greet(\"Ann\"), \"Hello, Ann!\")\n") &&
              write(".gitignore", "ixo_modules/\nbuild/\n") &&
              write("README.md", "# " + pkg + "\n\n```sh\nixo main.ixo          # run\nixo test              # run the tests\nixo run --watch main.ixo\n```\n") &&
              write(".vscode/launch.json",
                    "{\n  \"version\": \"0.2.0\",\n  \"configurations\": [\n    {\n      \"type\": \"ixo\",\n"
                    "      \"request\": \"launch\",\n      \"name\": \"Debug main.ixo\",\n"
                    "      \"program\": \"${workspaceFolder}/main.ixo\",\n      \"cwd\": \"${workspaceFolder}\"\n    }\n  ]\n}\n");
    if (!ok) {
        std::fprintf(stderr, "ixo new: can't write files in '%s'\n", name.c_str());
        return 1;
    }
    std::printf("created %s/\n  ixo.toml  main.ixo  lib/greeting.ixo  tests/greeting_test.ixo  README.md\n\n"
                "  cd %s\n  ixo main.ixo\n  ixo test\n",
                name.c_str(), name.c_str());
    return 0;
}

// ================================================================== formatter

namespace {

bool isValueEnd(const Token& t) {
    switch (t.kind) {
        case T::Int: case T::Float: case T::Str: case T::FStr: case T::Ident: case T::KwTrue:
        case T::KwFalse: case T::KwNil: case T::KwSelf: case T::RParen: case T::RBracket: case T::RBrace:
        case T::Question: case T::PlusPlus: case T::MinusMinus:
            return true;
        default: return false;
    }
}

bool isKeyword(T k) { return k >= T::KwFn && k <= T::KwTodo; }

struct Formatter {
    const std::string& src;
    std::vector<Token> toks;
    std::string out;
    int level = 0;
    int bracketDepth = 0;
    int pendingBlank = 0;  // blank lines recorded on INDENT/DEDENT tokens
    bool lineStart = true;
    size_t lineBegin = 0;  // offset in `out` of the current line
    std::vector<int> outCol;  // output column of each token already written
    int lineLevel = 0;        // indentation level of the current output line
    // blocks inside brackets (a lambda body in a call): indented from their header line
    struct Island {
        int depth;       // bracket depth
        int levelAt;     // `level` before the island's INDENT
        int baseLevel;   // indentation level of the header line
    };
    std::vector<Island> islands;

    // Indentation level for a line starting at bracket depth `depth`.
    int lineLevelFor(int depth, bool closing) {
        int lv;
        if (!islands.empty() && depth >= islands.back().depth) {
            const Island& is = islands.back();
            lv = is.baseLevel + (level - is.levelAt) + (depth - is.depth);
        } else {
            lv = level + depth;
        }
        if (closing && depth > 0) lv--;
        return lv;
    }

    // A NEWLINE the lexer adds before the closing bracket of a block that ends on
    // the same line (`_ -> "many")`): not a line break in the source.
    bool syntheticNewline(size_t i) {
        for (size_t j = i + 1; j < toks.size(); j++) {
            T k = toks[j].kind;
            if (k == T::Newline || k == T::Indent || k == T::Dedent) continue;
            if (k == T::Comment || k == T::Eof) return false;
            return !toks[j].newlineBefore;
        }
        return false;
    }

    // For an `elif` / `else` line continuing an `if` that started mid-line
    // (grade = if x > 8 -> "A"), the output column of that `if`; else -1.
    int inlineIfColumn(size_t i) {
        if (toks[i].kind != T::KwElif && toks[i].kind != T::KwElse) return -1;
        int nest = 0;
        bool skippedOwnIndent = false;
        for (size_t j = i; j-- > 0;) {
            T k = toks[j].kind;
            if (k == T::Dedent) {
                return -1;  // after a block: the if/else has block form
            } else if (k == T::Indent) {
                if (nest > 0) {
                    nest--;
                } else if (!skippedOwnIndent) {
                    skippedOwnIndent = true;
                } else {
                    return -1;  // left the enclosing block
                }
            } else if (k == T::KwIf && nest == 0) {
                bool atLineStart = j == 0 || toks[j - 1].kind == T::Newline || toks[j - 1].kind == T::Indent ||
                                   toks[j - 1].kind == T::Dedent;
                if (atLineStart) return -1;
                bool oneLine = false;  // if cond -> value
                for (size_t q = j + 1; q < i && toks[q].kind != T::Newline; q++)
                    if (toks[q].kind == T::Arrow) oneLine = true;
                if (!oneLine) return -1;
                return j < outCol.size() ? outCol[j] : -1;
            }
            if (i - j > 4000) return -1;
        }
        return -1;
    }
    std::vector<int> bracketLevels;

    explicit Formatter(const std::string& s) : src(s) {}

    std::string raw(const Token& t) const { return src.substr(t.start, t.end - t.start); }

    void newline() {
        while (!out.empty() && out.size() > lineBegin && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
        out += '\n';
        lineBegin = out.size();
        lineStart = true;
    }

    void blankLines(int n, int maxBlank) {
        n = std::min(n, maxBlank);
        // don't stack blank lines at the start of the file
        if (out.empty()) return;
        for (int i = 0; i < n; i++) {
            out += '\n';
            lineBegin = out.size();
        }
    }

    void indentTo(int lv) {
        out += std::string((size_t)std::max(lv, 0) * 4, ' ');
        lineStart = false;
    }

    // The level a standalone comment belongs to: the level of the next code line
    // when the comment isn't indented deeper than it.
    int commentLevel(size_t i) {
        int lv = level;
        for (size_t j = i + 1; j < toks.size(); j++) {
            if (toks[j].kind == T::Indent) {
                lv++;
                continue;
            }
            if (toks[j].kind == T::Dedent) {
                lv--;
                continue;
            }
            if (toks[j].kind == T::Comment || toks[j].kind == T::Newline) continue;
            if (toks[i].col <= toks[j].col || lv > level) return toks[i].col <= toks[j].col ? lv : level;
            return level;
        }
        return lv < level ? lv : level;
    }

    bool exportStar(size_t i) {
        // fn name* ... / class Name* ... / fn name(args)* -> T
        if (i < 1) return false;
        const Token& prev = toks[i - 1];
        const Token& next = i + 1 < toks.size() ? toks[i + 1] : toks.back();
        bool nextOk = next.kind == T::LParen || next.kind == T::Arrow || next.kind == T::Newline ||
                      next.kind == T::Assign || next.kind == T::Bang || next.kind == T::Eof || next.kind == T::Comment ||
                      next.kind == T::LBracket || next.kind == T::Colon || next.kind == T::KwIs;
        if (!nextOk) return false;
        if (prev.kind == T::Ident && i >= 2 &&
            (toks[i - 2].kind == T::KwFn || toks[i - 2].kind == T::KwClass || toks[i - 2].kind == T::KwInterface ||
             toks[i - 2].kind == T::KwEnum))
            return true;
        // limit* = 10: an exported module variable
        if (prev.kind == T::Ident && !toks[i].spaceBefore && (next.kind == T::Assign || next.kind == T::Colon) &&
            (i < 2 || toks[i - 2].kind == T::Newline || toks[i - 2].kind == T::Indent || toks[i - 2].kind == T::Dedent))
            return true;
        if (prev.kind == T::RParen) {
            // walk back to the start of the line: is it a fn/class header?
            for (size_t j = i; j-- > 0;) {
                if (toks[j].kind == T::Newline || toks[j].kind == T::Indent || toks[j].kind == T::Dedent) {
                    return toks[j + 1].kind == T::KwFn || toks[j + 1].kind == T::KwClass ||
                           toks[j + 1].kind == T::At || toks[j + 1].kind == T::Dollar || toks[j + 1].kind == T::KwFork ||
                           toks[j + 1].kind == T::KwPar;
                }
                if (j == 0)
                    return toks[0].kind == T::KwFn || toks[0].kind == T::KwClass || toks[0].kind == T::KwFork ||
                           toks[0].kind == T::KwPar;
            }
        }
        return false;
    }

    // `by` after a range: the step keyword (0..10 by 2), not a variable
    bool stepBy(size_t i) {
        if (toks[i].kind != T::Ident || toks[i].text != "by" || i == 0) return false;
        T pk = toks[i - 1].kind;
        return isValueEnd(toks[i - 1]) || pk == T::DotDot || pk == T::DotDotLess || pk == T::DotDotEq;
    }

    bool unaryAt(size_t i) {
        if (i == 0) return true;
        const Token& prev = toks[i - 1];
        if (prev.kind == T::Newline || prev.kind == T::Indent || prev.kind == T::Dedent) return true;
        if (stepBy(i - 1)) return true;
        return !isValueEnd(prev);
    }

    // Does toks[i] continue an operator method name (fn +(o), fn []=(i, v)), up to its '('?
    bool inOperatorName(size_t i) {
        size_t k = i;
        while (k > 0 && toks[k - 1].kind != T::KwFn) {
            T kk = toks[k - 1].kind;
            if (kk == T::Ident || kk == T::LParen || kk == T::RParen || kk == T::Newline || kk == T::Indent ||
                kk == T::Dedent || kk == T::Comment || kk == T::Comma || i - k >= 3)
                return false;
            k--;
        }
        if (k == 0 || k >= i) return false;
        switch (toks[k].kind) {  // the name's first token
            case T::Plus: case T::Minus: case T::Star: case T::Slash: case T::Percent: case T::StarStar:
            case T::EqEq: case T::Lt: case T::LBracket: return true;
            default: return false;
        }
    }

    bool spaceBetween(size_t i) {
        const Token& p = toks[i - 1];
        const Token& c = toks[i];
        T pk = p.kind, ck = c.kind;
        if (inOperatorName(i)) return false;
        if (pk == T::LParen || pk == T::LBracket || pk == T::LBrace) return false;
        if (ck == T::RParen || ck == T::RBracket || ck == T::RBrace) return false;
        if (ck == T::Comma || ck == T::Question || ck == T::PlusPlus || ck == T::MinusMinus) return false;
        if (pk == T::Dot || ck == T::Dot) return false;
        if ((pk == T::DotDot || pk == T::DotDotLess || pk == T::DotDotEq) && stepBy(i)) return true;
        if (pk == T::DotDot || pk == T::DotDotLess || pk == T::DotDotEq) return false;
        if (ck == T::DotDot || ck == T::DotDotLess || ck == T::DotDotEq) {
            // [first, ..rest] keeps its space after the comma
            return pk == T::Comma || pk == T::LBracket ? pk == T::Comma : false;
        }
        if (pk == T::At || pk == T::Dollar) return false;
        if (ck == T::Colon) return false;
        if (pk == T::Colon || pk == T::Comma) return true;
        if (ck == T::Star && exportStar(i)) return false;
        // fn name*= expr : keep the export marker glued to the name
        if (ck == T::StarAssign && ((pk == T::Ident && i >= 2 && toks[i - 2].kind == T::KwFn) || pk == T::RParen))
            return false;
        if (pk == T::Star && exportStar(i - 1)) return ck != T::LParen && ck != T::LBracket;
        // unary operators stick to their operand
        if ((pk == T::Minus || pk == T::Bang || pk == T::Tilde) && unaryAt(i - 1)) return false;
        if (ck == T::LParen) {
            if (pk == T::KwFn || pk == T::KwSelf) return false;
            if (isKeyword(pk)) return true;
            return !(pk == T::Ident || pk == T::RParen || pk == T::RBracket || pk == T::Str || pk == T::FStr);
        }
        if (ck == T::LBracket) {
            if (pk == T::Ident || pk == T::RParen || pk == T::RBracket || pk == T::Str || pk == T::KwSelf ||
                pk == T::Int || pk == T::Float || pk == T::FStr || pk == T::RBrace)
                return false;
            return true;
        }
        return true;
    }

    std::string run() {
        for (size_t i = 0; i < toks.size(); i++) {
            const Token& t = toks[i];
            switch (t.kind) {
                case T::Eof: break;
                case T::Newline:
                    if (bracketDepth > 0 && syntheticNewline(i)) break;
                    newline();
                    break;
                case T::Indent:
                    if (bracketDepth > 0 && (islands.empty() || islands.back().depth != bracketDepth ||
                                             level == islands.back().levelAt))
                        islands.push_back({bracketDepth, level, lineLevel});
                    level++;
                    pendingBlank = std::max(pendingBlank, t.blankLinesBefore);
                    break;
                case T::Dedent:
                    level--;
                    if (!islands.empty() && level == islands.back().levelAt) islands.pop_back();
                    pendingBlank = std::max(pendingBlank, t.blankLinesBefore);
                    break;
                case T::Comment: {
                    bool standalone = t.newlineBefore || lineStart;
                    if (standalone) {
                        if (!lineStart) newline();
                        blankLines(std::max(t.blankLinesBefore, pendingBlank), level == 0 ? 2 : 1);
                        pendingBlank = 0;
                        indentTo(commentLevel(i));
                        out += raw(t);
                        newline();
                    } else {
                        // trailing comment: keep its column if there's room, else two spaces
                        size_t curCol = out.size() - lineBegin;
                        size_t want = (size_t)(t.col - 1);
                        out += want > curCol + 1 ? std::string(want - curCol, ' ') : std::string("  ");
                        out += raw(t);
                    }
                    break;
                }
                default: {
                    if (lineStart) {
                        if (bracketDepth == 0) blankLines(std::max(t.blankLinesBefore, pendingBlank), level == 0 ? 2 : 1);
                        pendingBlank = 0;
                        bool closing = t.kind == T::RParen || t.kind == T::RBracket || t.kind == T::RBrace;
                        int lv = lineLevelFor(bracketDepth, closing);
                        lineLevel = lv;
                        int ifCol = bracketDepth == 0 ? inlineIfColumn(i) : -1;
                        if (ifCol >= 0) {
                            out += std::string((size_t)ifCol, ' ');
                            lineStart = false;
                        } else {
                            indentTo(lv);
                        }
                    } else if (bracketDepth > 0 && t.newlineBefore) {
                        newline();
                        bool closing = t.kind == T::RParen || t.kind == T::RBracket || t.kind == T::RBrace;
                        int lv = lineLevelFor(bracketDepth, closing);
                        lineLevel = lv;
                        indentTo(lv);
                    } else if (i > 0 && spaceBetween(i)) {
                        out += ' ';
                    }
                    if (outCol.size() <= i) outCol.resize(i + 1, -1);
                    outCol[i] = (int)(out.size() - lineBegin);
                    out += raw(t);
                    if (t.kind == T::LParen || t.kind == T::LBracket || t.kind == T::LBrace) bracketDepth++;
                    if ((t.kind == T::RParen || t.kind == T::RBracket || t.kind == T::RBrace) && bracketDepth > 0)
                        bracketDepth--;
                }
            }
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        out += '\n';
        return out;
    }
};

std::vector<std::pair<T, std::string>> signature(const std::string& s, bool* ok) {
    Diagnostics d;
    Lexer lx(s, "<fmt>", d, LexOptions{true});
    auto toks = lx.tokenize();
    *ok = !d.hasErrors();
    std::vector<std::pair<T, std::string>> out;
    for (auto& t : toks) {
        if (t.kind == T::Newline || t.kind == T::Indent || t.kind == T::Dedent) continue;
        out.push_back({t.kind, t.kind == T::Comment ? t.text : s.substr(t.start, t.end - t.start)});
    }
    return out;
}

}  // namespace

bool formatSource(const std::string& src, const std::string& file, std::string& out, std::string& error) {
    Diagnostics d;
    Lexer lx(src, file, d, LexOptions{true});
    auto toks = lx.tokenize();
    if (d.hasErrors()) {
        error = d.list[0].message + " (line " + std::to_string(d.list[0].pos.line) + ")";
        return false;
    }
    {
        Diagnostics pd;
        Lexer lx2(src, file, pd);
        Parser p(lx2.tokenize(), file, pd);
        p.parseProgram();
        if (pd.hasErrors()) {
            error = "syntax error: " + pd.list[0].message + " (line " + std::to_string(pd.list[0].pos.line) + ")";
            return false;
        }
    }
    Formatter f(src);
    f.toks = std::move(toks);
    out = f.run();
    bool ok1, ok2;
    auto before = signature(src, &ok1);
    auto after = signature(out, &ok2);
    if (!ok2 || before != after) {
        error = "internal formatter error: the result would change the program (please report this)";
        return false;
    }
    // layout must be preserved too
    Diagnostics a, b;
    Lexer l1(src, file, a), l2(out, file, b);
    auto t1 = l1.tokenize(), t2 = l2.tokenize();
    if (t1.size() != t2.size()) {
        error = "internal formatter error: indentation structure changed (please report this)";
        return false;
    }
    for (size_t i = 0; i < t1.size(); i++)
        if (t1[i].kind != t2[i].kind) {
            error = "internal formatter error: indentation structure changed at line " + std::to_string(t1[i].line);
            return false;
        }
    return true;
}

int applyFixes(std::string& src, const std::vector<const Diagnostic*>& diags) {
    // A fix is applied whole or not at all; fixes that overlap one already
    // taken wait for the next round.
    std::vector<TextEdit> taken;
    int applied = 0;
    auto overlaps = [&](const TextEdit& e) {
        for (auto& t : taken) {
            if (e.start == e.end && t.start == t.end) {
                if (e.start == t.start) return true;
                continue;
            }
            if (e.start < t.end && t.start < e.end) return true;
            if (e.start == e.end && e.start > t.start && e.start < t.end) return true;
            if (t.start == t.end && t.start > e.start && t.start < e.end) return true;
        }
        return false;
    };
    for (auto* d : diags) {
        if (d->fix.empty()) continue;
        bool ok = true;
        for (auto& e : d->fix)
            if (e.end < e.start || e.end > src.size() || overlaps(e)) ok = false;
        if (!ok) continue;
        for (auto& e : d->fix) taken.push_back(e);
        applied++;
    }
    std::sort(taken.begin(), taken.end(), [](const TextEdit& a, const TextEdit& b) {
        return a.start != b.start ? a.start > b.start : a.end > b.end;
    });
    for (auto& e : taken) src.replace(e.start, e.end - e.start, e.text);
    return applied;
}

int upgradeSource(const std::string& src, const std::string& file, std::string& out) {
    out = src;
    int total = 0;
    for (int round = 0; round < 10; round++) {
        Program prog;
        prog.opts.compile = false;
        std::string canon = canonicalPath(file);
        prog.overlays[canon] = out;
        Diagnostics diags;
        prog.loadFile(file, diags, prog.opts);
        std::vector<const Diagnostic*> mine;
        for (auto& d : diags.list)
            if (d.deprecated && !d.fix.empty() && canonicalPath(d.file) == canon) mine.push_back(&d);
        if (mine.empty()) break;
        int n = applyFixes(out, mine);
        if (n == 0) break;
        total += n;
    }
    return total;
}

int runFormatter(const std::vector<std::string>& args) {
    bool check = false, upgrade = false, noFormat = false;
    std::vector<std::string> files;
    for (auto& a : args) {
        if (a == "--check")
            check = true;
        else if (a == "--upgrade")
            upgrade = true;
        else if (a == "--no-format")
            noFormat = true;  // with --upgrade: only rewrite old spellings
        else
            collectFiles(a, files);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: ixo fmt [--check] [--upgrade] <files or dirs>   (use - for stdin)\n");
        return 2;
    }
    int changed = 0, failed = 0;
    for (auto& f : files) {
        std::string src;
        if (f == "-") {
            src.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        } else {
            bool ok = false;
            src = readFile(f, &ok);
            if (!ok) {
                std::fprintf(stderr, "ixo fmt: can't read %s\n", f.c_str());
                failed++;
                continue;
            }
        }
        std::string out, err;
        int upgraded = 0;
        if (upgrade) {
            std::string up;
            upgraded = upgradeSource(src, f == "-" ? "stdin.ixo" : f, up);
            if (upgraded || noFormat) {
                if (noFormat || !formatSource(up, f, out, err)) {
                    // keep the upgrade even if the formatter can't handle the result
                    out = up;
                    err.clear();
                }
                goto formatted;
            }
        }
        if (!formatSource(src, f, out, err)) {
            std::fprintf(stderr, "ixo fmt: %s: %s\n", f.c_str(), err.c_str());
            failed++;
            continue;
        }
    formatted:
        if (f == "-") {
            std::fwrite(out.data(), 1, out.size(), stdout);
            continue;
        }
        if (out == src) continue;
        changed++;
        if (check) {
            std::printf("would reformat %s\n", f.c_str());
        } else {
            FILE* fp = std::fopen(f.c_str(), "wb");
            if (!fp) {
                std::fprintf(stderr, "ixo fmt: can't write %s\n", f.c_str());
                failed++;
                continue;
            }
            std::fwrite(out.data(), 1, out.size(), fp);
            std::fclose(fp);
            if (upgraded)
                std::printf("upgraded %s (%d change%s)\n", f.c_str(), upgraded, upgraded == 1 ? "" : "s");
            else
                std::printf("formatted %s\n", f.c_str());
        }
    }
    if (failed) return 1;
    if (check && changed) return 1;
    return 0;
}

}  // namespace ixo
