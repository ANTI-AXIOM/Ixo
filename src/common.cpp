#include "common.hpp"

#include <fstream>
#include <sstream>

namespace ixo {

static std::string sourceLine(const std::string& src, int line) {
    int cur = 1;
    size_t start = 0;
    while (cur < line) {
        size_t nl = src.find('\n', start);
        if (nl == std::string::npos) return "";
        start = nl + 1;
        cur++;
    }
    size_t end = src.find('\n', start);
    std::string s = src.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!s.empty() && s.back() == '\r') s.pop_back();
    return s;
}

std::string formatDiagnostic(const Diagnostic& d, const std::string& source) {
    std::ostringstream o;
    o << d.file << ":" << d.pos.line << ":" << d.pos.col << ": "
      << (d.severity == Diagnostic::Error ? "error" : "warning") << ": " << d.message << "\n";
    std::string line = source.empty() ? "" : sourceLine(source, d.pos.line);
    if (!line.empty()) {
        std::string num = std::to_string(d.pos.line);
        o << "  " << num << " | " << line << "\n";
        o << "  " << std::string(num.size(), ' ') << " | ";
        for (int i = 1; i < d.pos.col && i <= (int)line.size(); i++) o << (line[i - 1] == '\t' ? '\t' : ' ');
        o << "^";
        for (int i = 1; i < d.length && d.pos.col + i <= (int)line.size(); i++) o << "~";
        o << "\n";
    }
    return o.str();
}

std::string readFile(const std::string& path, bool* ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (ok) *ok = false;
        return "";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (ok) *ok = true;
    return ss.str();
}

}  // namespace ixo
