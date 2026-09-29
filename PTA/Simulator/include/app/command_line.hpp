#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "analysis/pta_analysis.hpp"

namespace simulator::app {

struct CommandLineOptions {
    std::string inputPath;
    std::optional<std::filesystem::path> outputRoot;
    // Compatibility mirrors. Structured artifacts are always generated;
    // these paths optionally receive additional listing/raw copies.
    std::optional<std::string> listingPath;
    std::optional<std::string> rawPath;
    std::optional<std::filesystem::path> configPath;
    std::size_t configSearchDepth = 8;
    bool printConfigPath = false;
    PtaMode ptaMode = PtaMode::Disabled;
    bool ptaModeSpecified = false;
    bool ptaDiagnostics = false;
    // An absent value lets config.json select the default.  A supplied CLI
    // value takes precedence over the configuration file.
    std::optional<bool> debug;
    std::vector<std::string> operatingPointOptionAssignments;
    std::vector<std::string> ptaOptionAssignments;
    std::vector<std::string> transientOptionAssignments;
    bool batchMode = false;
    bool parseOnly = false;
    bool helpRequested = false;
};

void printUsage(std::ostream& output, const char* program);

bool parseCommandLine(int argc,
                      char* argv[],
                      CommandLineOptions& options,
                      std::ostream& output,
                      std::ostream& error);

}  // namespace simulator::app
