#pragma once

#include <string>
#include <string_view>

class Circuit;
struct PtaAnalysisConfig;

// Stable JSONL artifact for PTA experiments.  Each line is independent so a
// partially inspected run can still be streamed or recovered by tooling.
class PtaTraceWriter {
public:
    static std::string write(
        const Circuit& circuit,
        const PtaAnalysisConfig& config,
        std::string_view inputPath,
        std::string_view configurationSource,
        std::string_view status,
        std::string_view statusDetail
    );
};
