// A small TOML reader for ixo.toml: [sections], key = value with strings,
// numbers, booleans, arrays and inline tables ({ git = "...", rev = "..." }).
#pragma once

#include <map>
#include <string>
#include <vector>

namespace ixo {

struct TomlValue {
    enum K { Str, Num, Bool, Arr, Table } k = Str;
    std::string s;
    double n = 0;
    bool b = false;
    std::vector<TomlValue> arr;
    std::map<std::string, TomlValue> table;
    std::string str(const std::string& key, const std::string& def = "") const {
        auto it = table.find(key);
        return it != table.end() && it->second.k == Str ? it->second.s : def;
    }
};

struct TomlDoc {
    std::map<std::string, std::map<std::string, TomlValue>> sections;  // "" = top level
    std::vector<std::string> sectionOrder;
    const TomlValue* get(const std::string& section, const std::string& key) const;
    std::string str(const std::string& section, const std::string& key, const std::string& def = "") const;
};

// Returns false with an error ("line 3: ...") on malformed input.
bool parseToml(const std::string& text, TomlDoc& out, std::string* error);
std::string tomlQuote(const std::string& s);

// The nearest directory at or above `dir` containing ixo.toml ("" if none).
std::string findProjectRoot(const std::string& dir);

}  // namespace ixo
