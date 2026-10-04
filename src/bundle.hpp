// `ixo build`: a program and its modules packed into one executable.
//
// The output is a copy of the ixo executable with the sources appended:
//     [ixo] [files] [entry] [payload size: u64] ["IXOBNDL1"]
// At startup ixo looks for that trailer; when it's there, it runs the bundled
// entry with every command-line argument passed to the program. The bundled
// files appear under a virtual folder next to the executable, so module lookup
// (including ixo.toml and ixo_modules/) works as it did in the project.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ixo {

struct Bundle {
    std::string root;                          // the virtual folder (canonical)
    std::string entry;                         // canonical path of the main file
    std::map<std::string, std::string> files;  // canonical path -> source
    std::set<std::string> dirs;                // every folder that holds a bundled file
};

// The bundle in the running executable, or null.
const Bundle* activeBundle();
bool loadBundleFromSelf();

// For module lookup: a real file/folder, or one inside the bundle.
bool vfsIsFile(const std::string& path);
bool vfsIsDir(const std::string& path);
bool vfsRead(const std::string& path, std::string& out);

int runBuild(const std::vector<std::string>& args);

}  // namespace ixo
