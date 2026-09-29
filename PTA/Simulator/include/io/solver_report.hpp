#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include "analysis/solver_diagnostics.hpp"

class Circuit;
struct AnalysisPlan;
struct OperatingPointSolverOptions;
struct PtaAnalysisConfig;

struct OperatingPointAttemptReport {
    std::string label;
    OperatingPointDiagnostics diagnostics;
};

struct SimulationReport {
    std::string inputPath;
    std::string title;
    std::string configurationSource;
    std::string status = "failed";
    std::string statusDetail;
    double totalWallSeconds = 0.0;
    std::vector<OperatingPointAttemptReport> operatingPointAttempts;
    std::optional<TransientDiagnostics> transient;
};

class SolverReportWriter {
public:
    static void write(
        std::ostream& output,
        const SimulationReport& report,
        const Circuit* circuit,
        const AnalysisPlan* plan,
        const OperatingPointSolverOptions* operatingPointConfig,
        const PtaAnalysisConfig* ptaConfig
    );
};
