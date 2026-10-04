// Line editing for the REPL: cursor movement, history (saved between
// sessions), tab completion. Falls back to plain line reading when the input
// isn't a terminal.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ixo {

class LineEditor {
public:
    explicit LineEditor(std::string historyFile);
    ~LineEditor();

    enum Result { Line, Cancelled, End };  // Cancelled: Ctrl+C; End: Ctrl+D / end of input
    Result read(const std::string& prompt, std::string& out);
    void addHistory(const std::string& line);

    // Given the text before the cursor, returns (start of the word being completed, candidates).
    std::function<std::pair<size_t, std::vector<std::string>>(const std::string& before)> completer;

    bool interactive() const { return tty; }

private:
    bool tty = false;
    std::string historyFile;
    std::vector<std::string> history;
    struct Impl;
    Impl* impl;

    Result readPlain(const std::string& prompt, std::string& out);
    Result readRaw(const std::string& prompt, std::string& out);
    void saveHistory();
};

}  // namespace ixo
