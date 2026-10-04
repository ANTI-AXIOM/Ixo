#include "lineedit.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace ixo {

namespace {

// Keys, decoded from the platform's input.
enum class Key { Char, Enter, Backspace, Delete, Left, Right, Home, End, Up, Down, Tab, CtrlC, CtrlD, CtrlK, CtrlU,
                 CtrlW, CtrlL, CtrlLeft, CtrlRight, Eof, Other };

struct KeyEvent {
    Key key = Key::Other;
    std::string text;  // Key::Char: UTF-8 of the character
};

bool isCont(unsigned char c) { return (c & 0xC0) == 0x80; }

size_t prevChar(const std::string& s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && isCont((unsigned char)s[i])) i--;
    return i;
}
size_t nextChar(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    i++;
    while (i < s.size() && isCont((unsigned char)s[i])) i++;
    return i;
}
size_t width(const std::string& s) {
    size_t w = 0;
    for (unsigned char c : s)
        if (!isCont(c)) w++;
    return w;
}
bool wordChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

void out(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

}  // namespace

#ifdef _WIN32

struct LineEditor::Impl {
    HANDLE in = nullptr, outH = nullptr;
    DWORD oldIn = 0, oldOut = 0;
    wchar_t pendingHigh = 0;

    KeyEvent readKey() {
        while (true) {
            INPUT_RECORD rec;
            DWORD got = 0;
            if (!ReadConsoleInputW(in, &rec, 1, &got) || got == 0) return {Key::Eof, ""};
            if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
            auto& k = rec.Event.KeyEvent;
            bool ctrl = (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
            switch (k.wVirtualKeyCode) {
                case VK_RETURN: return {Key::Enter, ""};
                case VK_BACK: return {Key::Backspace, ""};
                case VK_DELETE: return {Key::Delete, ""};
                case VK_LEFT: return {ctrl ? Key::CtrlLeft : Key::Left, ""};
                case VK_RIGHT: return {ctrl ? Key::CtrlRight : Key::Right, ""};
                case VK_HOME: return {Key::Home, ""};
                case VK_END: return {Key::End, ""};
                case VK_UP: return {Key::Up, ""};
                case VK_DOWN: return {Key::Down, ""};
                case VK_TAB: return {Key::Tab, ""};
                default: break;
            }
            wchar_t ch = k.uChar.UnicodeChar;
            if (ctrl) {
                switch (k.wVirtualKeyCode) {
                    case 'C': return {Key::CtrlC, ""};
                    case 'D': return {Key::CtrlD, ""};
                    case 'K': return {Key::CtrlK, ""};
                    case 'U': return {Key::CtrlU, ""};
                    case 'W': return {Key::CtrlW, ""};
                    case 'L': return {Key::CtrlL, ""};
                    case 'A': return {Key::Home, ""};
                    case 'E': return {Key::End, ""};
                    case 'Z': return {Key::CtrlD, ""};
                    default: break;
                }
            }
            if (ch == 0 || ch < 32) continue;
            if (ch >= 0xD800 && ch <= 0xDBFF) {
                pendingHigh = ch;
                continue;
            }
            wchar_t buf[2] = {ch, 0};
            int n = 1;
            if (ch >= 0xDC00 && ch <= 0xDFFF && pendingHigh) {
                buf[0] = pendingHigh;
                buf[1] = ch;
                n = 2;
                pendingHigh = 0;
            }
            char utf8[8];
            int len = WideCharToMultiByte(CP_UTF8, 0, buf, n, utf8, sizeof utf8, nullptr, nullptr);
            return {Key::Char, std::string(utf8, (size_t)std::max(len, 0))};
        }
    }
    void raw() {
        GetConsoleMode(in, &oldIn);
        SetConsoleMode(in, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT);
        GetConsoleMode(outH, &oldOut);
        SetConsoleMode(outH, oldOut | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    }
    void restore() {
        SetConsoleMode(in, oldIn);
        SetConsoleMode(outH, oldOut);
    }
};

static bool stdinIsTerminal() {
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0 &&
           GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
}

static std::string homeDir() {
    const char* h = std::getenv("USERPROFILE");
    return h ? h : ".";
}

#else

struct LineEditor::Impl {
    termios old{};

    int getc() {
        unsigned char c;
        return ::read(0, &c, 1) == 1 ? c : -1;
    }
    KeyEvent readKey() {
        int c = getc();
        if (c < 0) return {Key::Eof, ""};
        switch (c) {
            case '\r':
            case '\n': return {Key::Enter, ""};
            case 127:
            case 8: return {Key::Backspace, ""};
            case '\t': return {Key::Tab, ""};
            case 3: return {Key::CtrlC, ""};
            case 4: return {Key::CtrlD, ""};
            case 11: return {Key::CtrlK, ""};
            case 21: return {Key::CtrlU, ""};
            case 23: return {Key::CtrlW, ""};
            case 12: return {Key::CtrlL, ""};
            case 1: return {Key::Home, ""};
            case 5: return {Key::End, ""};
            case 27: {
                int a = getc();
                if (a != '[' && a != 'O') return {Key::Other, ""};
                int b = getc();
                if (b >= '0' && b <= '9') {
                    std::string seq(1, (char)b);
                    int x;
                    while ((x = getc()) >= 0 && x != '~' && !std::isalpha(x)) seq += (char)x;
                    if (x == '~') {
                        if (seq == "3") return {Key::Delete, ""};
                        if (seq == "1" || seq == "7") return {Key::Home, ""};
                        if (seq == "4" || seq == "8") return {Key::End, ""};
                    }
                    if (seq == "1;5" && x == 'C') return {Key::CtrlRight, ""};
                    if (seq == "1;5" && x == 'D') return {Key::CtrlLeft, ""};
                    return {Key::Other, ""};
                }
                switch (b) {
                    case 'A': return {Key::Up, ""};
                    case 'B': return {Key::Down, ""};
                    case 'C': return {Key::Right, ""};
                    case 'D': return {Key::Left, ""};
                    case 'H': return {Key::Home, ""};
                    case 'F': return {Key::End, ""};
                    default: return {Key::Other, ""};
                }
            }
            default: break;
        }
        if (c < 32) return {Key::Other, ""};
        std::string s(1, (char)c);
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        for (int i = 0; i < extra; i++) {
            int d = getc();
            if (d < 0) break;
            s += (char)d;
        }
        return {Key::Char, s};
    }
    void raw() {
        tcgetattr(0, &old);
        termios t = old;
        t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        t.c_iflag &= ~(IXON | ICRNL);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSAFLUSH, &t);
    }
    void restore() { tcsetattr(0, TCSAFLUSH, &old); }
};

static bool stdinIsTerminal() { return isatty(0) && isatty(1); }

static std::string homeDir() {
    const char* h = std::getenv("HOME");
    return h ? h : ".";
}

#endif

LineEditor::LineEditor(std::string file) : impl(new Impl()) {
    tty = stdinIsTerminal() && !std::getenv("IXO_NO_LINEEDIT");
#ifdef _WIN32
    impl->in = GetStdHandle(STD_INPUT_HANDLE);
    impl->outH = GetStdHandle(STD_OUTPUT_HANDLE);
#endif
    historyFile = file.empty() ? homeDir() + "/.ixo_history" : file;
    std::ifstream f(historyFile);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // multi-line entries are stored with \n escaped
        std::string decoded;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                decoded += line[i + 1] == 'n' ? '\n' : line[i + 1];
                i++;
            } else {
                decoded += line[i];
            }
        }
        if (!decoded.empty()) history.push_back(decoded);
    }
}

LineEditor::~LineEditor() { delete impl; }

void LineEditor::addHistory(const std::string& line) {
    if (line.empty() || (!history.empty() && history.back() == line)) return;
    history.push_back(line);
    if (history.size() > 1000) history.erase(history.begin(), history.begin() + (long)(history.size() - 1000));
    saveHistory();
}

void LineEditor::saveHistory() {
    if (!tty) return;
    std::ofstream f(historyFile, std::ios::binary);
    for (auto& h : history) {
        std::string enc;
        for (char c : h) {
            if (c == '\n') enc += "\\n";
            else if (c == '\\') enc += "\\\\";
            else enc += c;
        }
        f << enc << "\n";
    }
}

LineEditor::Result LineEditor::read(const std::string& prompt, std::string& line) {
    return tty ? readRaw(prompt, line) : readPlain(prompt, line);
}

LineEditor::Result LineEditor::readPlain(const std::string& prompt, std::string& line) {
    out(prompt);
    if (!std::getline(std::cin, line)) return End;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return Line;
}

LineEditor::Result LineEditor::readRaw(const std::string& prompt, std::string& result) {
    std::string buf;
    size_t cur = 0;
    size_t histPos = history.size();
    std::string saved;  // what was typed before browsing history
    impl->raw();
    auto redraw = [&] {
        std::string s = "\r" + prompt + buf + "\x1b[K\r";
        size_t col = width(prompt) + width(buf.substr(0, cur));
        if (col > 0) s += "\x1b[" + std::to_string(col) + "C";
        out(s);
    };
    redraw();
    Result r = Line;
    while (true) {
        KeyEvent ev = impl->readKey();
        switch (ev.key) {
            case Key::Char:
                buf.insert(cur, ev.text);
                cur += ev.text.size();
                break;
            case Key::Enter: goto done;
            case Key::Backspace:
                if (cur > 0) {
                    size_t p = prevChar(buf, cur);
                    buf.erase(p, cur - p);
                    cur = p;
                }
                break;
            case Key::Delete:
                if (cur < buf.size()) buf.erase(cur, nextChar(buf, cur) - cur);
                break;
            case Key::Left: cur = prevChar(buf, cur); break;
            case Key::Right: cur = nextChar(buf, cur); break;
            case Key::CtrlLeft:
                while (cur > 0 && !wordChar(buf[cur - 1])) cur = prevChar(buf, cur);
                while (cur > 0 && wordChar(buf[cur - 1])) cur = prevChar(buf, cur);
                break;
            case Key::CtrlRight:
                while (cur < buf.size() && !wordChar(buf[cur])) cur = nextChar(buf, cur);
                while (cur < buf.size() && wordChar(buf[cur])) cur = nextChar(buf, cur);
                break;
            case Key::Home: cur = 0; break;
            case Key::End: cur = buf.size(); break;
            case Key::Up:
                if (histPos > 0) {
                    if (histPos == history.size()) saved = buf;
                    histPos--;
                    buf = history[histPos];
                    // a multi-line entry is recalled one line at a time: show its first line
                    size_t nl = buf.find('\n');
                    if (nl != std::string::npos) buf = buf.substr(0, nl);
                    cur = buf.size();
                }
                break;
            case Key::Down:
                if (histPos < history.size()) {
                    histPos++;
                    buf = histPos == history.size() ? saved : history[histPos];
                    size_t nl = buf.find('\n');
                    if (nl != std::string::npos) buf = buf.substr(0, nl);
                    cur = buf.size();
                }
                break;
            case Key::CtrlK: buf.erase(cur); break;
            case Key::CtrlU:
                buf.erase(0, cur);
                cur = 0;
                break;
            case Key::CtrlW: {
                size_t p = cur;
                while (p > 0 && buf[p - 1] == ' ') p--;
                while (p > 0 && buf[p - 1] != ' ') p--;
                buf.erase(p, cur - p);
                cur = p;
                break;
            }
            case Key::CtrlL: out("\x1b[2J\x1b[H"); break;
            case Key::CtrlC:
                out("^C");
                r = Cancelled;
                buf.clear();
                goto done;
            case Key::CtrlD:
                if (buf.empty()) {
                    r = End;
                    goto done;
                }
                if (cur < buf.size()) buf.erase(cur, nextChar(buf, cur) - cur);
                break;
            case Key::Eof:
                r = End;
                goto done;
            case Key::Tab: {
                if (!completer) {
                    buf.insert(cur, "    ");
                    cur += 4;
                    break;
                }
                std::string before = buf.substr(0, cur);
                // at the start of a line (only spaces before): indent
                if (before.find_first_not_of(' ') == std::string::npos) {
                    buf.insert(cur, "    ");
                    cur += 4;
                    break;
                }
                auto [start, cands] = completer(before);
                if (cands.empty()) break;
                std::string typed = before.substr(start);
                std::string common = cands[0];
                for (auto& c : cands) {
                    size_t k = 0;
                    while (k < common.size() && k < c.size() && common[k] == c[k]) k++;
                    common.resize(k);
                }
                if (common.size() > typed.size()) {
                    buf.replace(start, cur - start, common);
                    cur = start + common.size();
                } else if (cands.size() > 1) {
                    std::string list = "\r\n";
                    size_t shown = 0;
                    for (auto& c : cands) {
                        if (shown++ >= 60) {
                            list += "...";
                            break;
                        }
                        list += c + "  ";
                    }
                    out(list + "\r\n");
                }
                break;
            }
            case Key::Other: break;
        }
        redraw();
    }
done:
    out("\r\n");
    impl->restore();
    result = buf;
    return r;
}

}  // namespace ixo
