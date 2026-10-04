// Shared utilities: source positions, diagnostics, small helpers.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace ixo {

// Ends the process now, without waiting for par worker threads.
[[noreturn]] inline void hardExit(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef __EMSCRIPTEN__
    emscripten_force_exit(code);  // _Exit from a thread doesn't end the program there
#endif
    std::_Exit(code);
}

struct SrcPos {
    int line = 0;
    int col = 0;
};

// A replacement of the source bytes [start, end) (an insertion when they're equal).
struct TextEdit {
    size_t start = 0, end = 0;
    std::string text;
};

struct Diagnostic {
    enum Severity { Error, Warning };
    Severity severity = Error;
    std::string file;
    SrcPos pos;
    int length = 1;  // number of columns to underline (LSP range)
    std::string message;
    // An automatic fix (quick fix in editors; `ixo fmt --upgrade` applies the
    // fixes of deprecation warnings).
    std::vector<TextEdit> fix;
    bool deprecated = false;  // an old spelling that still works
};

// Collects diagnostics for one compilation. Parsing and checking keep going
// after an error so tools (the LSP, `ixo check`) can report several at once.
class Diagnostics {
public:
    void error(const std::string& file, SrcPos pos, std::string msg, int length = 1) {
        list.push_back({Diagnostic::Error, file, pos, length, std::move(msg), {}, false});
        errorCount++;
    }
    void warning(const std::string& file, SrcPos pos, std::string msg, int length = 1,
                 std::vector<TextEdit> fix = {}) {
        list.push_back({Diagnostic::Warning, file, pos, length, std::move(msg), std::move(fix), false});
    }
    // An old spelling: a warning with the fix that writes the new one.
    void deprecated(const std::string& file, SrcPos pos, std::string msg, int length,
                    std::vector<TextEdit> fix) {
        if (quietDeprecations) return;
        list.push_back({Diagnostic::Warning, file, pos, length, "deprecated: " + std::move(msg), std::move(fix), true});
    }
    bool quietDeprecations = false;
    bool hasErrors() const { return errorCount > 0; }
    std::vector<Diagnostic> list;
    int errorCount = 0;
};

// Formats "file:line:col: error: message" plus the offending source line.
std::string formatDiagnostic(const Diagnostic& d, const std::string& source);

std::string readFile(const std::string& path, bool* ok);

}  // namespace ixo
