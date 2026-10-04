#include "process.hpp"

#include <cerrno>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <cstring>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace ixo {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Quoting that CommandLineToArgvW (and the C runtime) undo.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    for (size_t i = 0;; i++) {
        size_t backslashes = 0;
        while (i < a.size() && a[i] == L'\\') {
            i++;
            backslashes++;
        }
        if (i == a.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out += a[i];
    }
    return out + L"\"";
}

std::string lastError() {
    DWORD e = GetLastError();
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   0, (LPWSTR)&buf, 0, nullptr);
    std::string s = buf ? narrow(buf) : "error " + std::to_string(e);
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}

std::string readHandle(HANDLE h) {
    std::string out;
    if (!h) return out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) out.append(buf, got);
    return out;
}

class WinProcess : public Process {
public:
    PROCESS_INFORMATION pi{};
    HANDLE in = nullptr, out = nullptr, err = nullptr;
    int exitCode = -1;
    bool done = false;

    ~WinProcess() override {
        if (in) CloseHandle(in);
        if (out) CloseHandle(out);
        if (err) CloseHandle(err);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
    }
    bool running() override { return !done && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT; }
    int wait() override {
        if (!done) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            exitCode = (int)code;
            done = true;
        }
        return exitCode;
    }
    void kill() override {
        if (running()) TerminateProcess(pi.hProcess, 1);
        wait();
    }
    bool write(const std::string& data) override {
        if (!in) return false;
        DWORD put = 0;
        return WriteFile(in, data.data(), (DWORD)data.size(), &put, nullptr) && put == data.size();
    }
    void closeInput() override {
        if (in) CloseHandle(in);
        in = nullptr;
    }
    std::string readAllOut() override { return readHandle(out); }
    std::string readAllErr() override { return readHandle(err); }
};

}  // namespace

std::unique_ptr<Process> Process::spawn(const std::vector<std::string>& argv, const ProcessOptions& opts, std::string* error) {
    if (argv.empty()) {
        if (error) *error = "no program to run";
        return nullptr;
    }
    auto p = std::make_unique<WinProcess>();
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE childIn = nullptr, childOut = nullptr, childErr = nullptr;
    if (opts.pipeInput) {
        CreatePipe(&childIn, &p->in, &sa, 0);
        SetHandleInformation(p->in, HANDLE_FLAG_INHERIT, 0);
    }
    if (opts.capture) {
        CreatePipe(&p->out, &childOut, &sa, 0);
        SetHandleInformation(p->out, HANDLE_FLAG_INHERIT, 0);
        CreatePipe(&p->err, &childErr, &sa, 0);
        SetHandleInformation(p->err, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (opts.pipeInput || opts.capture) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = opts.pipeInput ? childIn : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = opts.capture ? childOut : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = opts.capture ? childErr : GetStdHandle(STD_ERROR_HANDLE);
    }
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); i++) cmd += (i ? L" " : L"") + quoteArg(widen(argv[i]));
    // find the program on PATH (also PATHEXT: .exe, .cmd, .bat)
    std::wstring exe = widen(argv[0]);
    wchar_t found[MAX_PATH];
    std::wstring app;
    if (SearchPathW(nullptr, exe.c_str(), L".exe", MAX_PATH, found, nullptr)) app = found;
    std::wstring cwd = widen(opts.cwd);
    BOOL ok = CreateProcessW(app.empty() ? nullptr : app.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &p->pi);
    std::string why = ok ? "" : lastError();
    if (childIn) CloseHandle(childIn);
    if (childOut) CloseHandle(childOut);
    if (childErr) CloseHandle(childErr);
    if (!ok) {
        if (error) *error = "can't run '" + argv[0] + "': " + why;
        return nullptr;
    }
    return p;
}

std::string selfExecutable() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    return narrow(std::wstring(buf, n));
}

#else

namespace {

std::string readFd(int fd) {
    std::string out;
    if (fd < 0) return out;
    char buf[4096];
    ssize_t got;
    while ((got = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, (size_t)got);
    return out;
}

class PosixProcess : public Process {
public:
    pid_t pid = -1;
    int in = -1, out = -1, err = -1;
    int exitCode = -1;
    bool done = false;

    ~PosixProcess() override {
        if (in >= 0) ::close(in);
        if (out >= 0) ::close(out);
        if (err >= 0) ::close(err);
    }
    bool running() override {
        if (done) return false;
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == 0) return true;
        record(status);
        return false;
    }
    void record(int status) {
        done = true;
        exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    }
    int wait() override {
        if (!done) {
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            record(status);
        }
        return exitCode;
    }
    void kill() override {
        if (running()) ::kill(pid, SIGTERM);
        wait();
    }
    bool write(const std::string& data) override {
        if (in < 0) return false;
        size_t put = 0;
        while (put < data.size()) {
            ssize_t n = ::write(in, data.data() + put, data.size() - put);
            if (n <= 0) return false;
            put += (size_t)n;
        }
        return true;
    }
    void closeInput() override {
        if (in >= 0) ::close(in);
        in = -1;
    }
    std::string readAllOut() override { return readFd(out); }
    std::string readAllErr() override { return readFd(err); }
};

}  // namespace

std::unique_ptr<Process> Process::spawn(const std::vector<std::string>& argv, const ProcessOptions& opts, std::string* error) {
    if (argv.empty()) {
        if (error) *error = "no program to run";
        return nullptr;
    }
    auto p = std::make_unique<PosixProcess>();
    int inPipe[2] = {-1, -1}, outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
    if (opts.pipeInput && pipe(inPipe) != 0) return nullptr;
    if (opts.capture && (pipe(outPipe) != 0 || pipe(errPipe) != 0)) return nullptr;
    int execFail[2];
    if (pipe(execFail) != 0) return nullptr;
    fcntl(execFail[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = std::string("can't start a process: ") + std::strerror(errno);
        return nullptr;
    }
    if (pid == 0) {
        if (opts.pipeInput) {
            dup2(inPipe[0], 0);
            ::close(inPipe[0]);
            ::close(inPipe[1]);
        }
        if (opts.capture) {
            dup2(outPipe[1], 1);
            dup2(errPipe[1], 2);
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            ::close(errPipe[0]);
            ::close(errPipe[1]);
        }
        ::close(execFail[0]);
        if (!opts.cwd.empty() && chdir(opts.cwd.c_str()) != 0) {
            int e = errno;
            (void)!::write(execFail[1], &e, sizeof e);
            _exit(127);
        }
        std::vector<char*> args;
        for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        int e = errno;
        (void)!::write(execFail[1], &e, sizeof e);
        _exit(127);
    }
    ::close(execFail[1]);
    int e = 0;
    ssize_t got = ::read(execFail[0], &e, sizeof e);
    ::close(execFail[0]);
    if (opts.pipeInput) {
        ::close(inPipe[0]);
        p->in = inPipe[1];
    }
    if (opts.capture) {
        ::close(outPipe[1]);
        ::close(errPipe[1]);
        p->out = outPipe[0];
        p->err = errPipe[0];
    }
    p->pid = pid;
    if (got == (ssize_t)sizeof e) {
        p->wait();
        if (error) *error = "can't run '" + argv[0] + "': " + std::strerror(e);
        return nullptr;
    }
    return p;
}

std::string selfExecutable() {
    char buf[PATH_MAX];
#ifdef __APPLE__
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real[PATH_MAX];
        return realpath(buf, real) ? std::string(real) : std::string(buf);
    }
#endif
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) return std::string(buf, (size_t)n);
    return "ixo";
}

#endif

RunResult runProcess(const std::vector<std::string>& argv, const std::string& input, const std::string& cwd) {
    RunResult r;
    ProcessOptions o;
    o.capture = true;
    o.pipeInput = true;
    o.cwd = cwd;
    auto p = Process::spawn(argv, o, &r.error);
    if (!p) return r;
    r.started = true;
    std::thread outT([&] { r.out = p->readAllOut(); });
    std::thread errT([&] { r.err = p->readAllErr(); });
    p->write(input);
    p->closeInput();
    outT.join();
    errT.join();
    r.code = p->wait();
    return r;
}

}  // namespace ixo
