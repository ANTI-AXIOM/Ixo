// Child processes: used by watch mode, the `proc` standard module and tools.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace ixo {

struct ProcessOptions {
    std::string cwd;           // empty: the current directory
    bool capture = false;      // pipe stdout and stderr (else inherited)
    bool pipeInput = false;    // pipe stdin (else inherited)
};

class Process {
public:
    virtual ~Process() = default;
    // Starts argv[0] (looked up on PATH) with the arguments. Null on failure (see *error).
    static std::unique_ptr<Process> spawn(const std::vector<std::string>& argv, const ProcessOptions& opts,
                                          std::string* error);
    virtual bool running() = 0;
    virtual int wait() = 0;  // exit code (128 + signal on POSIX when killed by a signal)
    virtual void kill() = 0;
    // With capture/pipeInput:
    virtual bool write(const std::string& data) = 0;
    virtual void closeInput() = 0;
    // Reads stdout / stderr until the process closes them (blocking; run on separate threads).
    virtual std::string readAllOut() = 0;
    virtual std::string readAllErr() = 0;
};

struct RunResult {
    bool started = false;
    std::string error;  // why it didn't start
    int code = -1;
    std::string out, err;
};

// Runs a program to completion, feeding `input` and capturing its output.
RunResult runProcess(const std::vector<std::string>& argv, const std::string& input, const std::string& cwd = "");

std::string selfExecutable();  // path of the running ixo executable

}  // namespace ixo
