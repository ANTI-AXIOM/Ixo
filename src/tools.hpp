// Developer tools built into the ixo executable.
#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace ixo {

int runRepl();
int runFormatter(const std::vector<std::string>& args);
int runTests(const std::vector<std::string>& args);
int runLanguageServer();
// Runs `ixo <childArgs>` and restarts it whenever a watched file changes.
// `targets` are the files/dirs to watch (with the modules they import).
int runWatch(const std::vector<std::string>& childArgs, const std::vector<std::string>& targets);
int runNewProject(const std::vector<std::string>& args);
// the package manager (pkg.cpp)
int runInit(const std::vector<std::string>& args);
int runAdd(const std::vector<std::string>& args);
int runInstall(const std::vector<std::string>& args);
int runUpdate(const std::vector<std::string>& args);
int runRemove(const std::vector<std::string>& args);

// Formats Ixo source. Returns false (with an error message) if the source
// doesn't lex cleanly or formatting would change its meaning.
bool formatSource(const std::string& src, const std::string& file, std::string& out, std::string& error);
// Applies the fixes of every deprecation warning (old spellings -> new ones).
// Returns the number of fixes applied.
int upgradeSource(const std::string& src, const std::string& file, std::string& out);
// Applies the non-overlapping fixes of `diags` to `src`; returns how many.
int applyFixes(std::string& src, const std::vector<const Diagnostic*>& diags);

}  // namespace ixo
