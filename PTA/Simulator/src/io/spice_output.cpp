#include "io/spice_output.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "analysis/analysis_plan.hpp"
#include "analysis/pta_analysis.hpp"
#include "circuit/circuit.hpp"
#include "circuit/node_map.hpp"
#include "devices/device.hpp"
#include "solver/mna.hpp"
#include "utils/string.hpp"

class SpiceOutputAccess {
public:
    static const auto& devices(const Circuit& circuit){
        return circuit.devices_;
    }

    static const NodeMap& nodeMap(const Circuit& circuit){
        return *circuit.nodeMap_;
    }

    static const MNA& mna(const Circuit& circuit){
        return *circuit.mna_;
    }

    static const auto& transientSamples(const Circuit& circuit){
        return circuit.transientSamples_;
    }
};

namespace {
constexpr int kIndexWidth = 8;
constexpr int kValueWidth = 20;

const char* ptaIntegrationMethodName(int order){
    switch(order){
        case 1: return "BE";
        case 2: return "BDF2";
        default: return "unknown";
    }
}

const char* ptaAttemptDecision(const PtaStepAttemptDiagnostics& attempt){
    if(attempt.reachedSteadyState) return "steady-state";
    if(attempt.restartedAfterCapacitanceGrowth){
        return "capacitance-growth-restart";
    }
    if(attempt.retriedWithSmallerStep) return "retry-smaller-step";
    if(attempt.accepted) return "accepted";
    return "failed";
}

struct ResolvedVariable {
    PrintQuantity quantity = PrintQuantity::Voltage;
    std::string label;
    std::string rawLabel;
    std::string rawType;
    int index = -1;
    int referenceIndex = -1;
};

int branchIndexOf(const Circuit& circuit, const std::string& deviceName){
    for(const auto& device: SpiceOutputAccess::devices(circuit)){
        if(equal_ignore_case(device->getName(), deviceName)){
            const int branch = device->branchUnknown();
            if(branch < 0){
                throw std::runtime_error(
                    "Current output is unavailable for device " + deviceName +
                    "; only devices with a branch unknown are supported"
                );
            }
            return branch;
        }
    }
    throw std::runtime_error("Unknown output device: " + deviceName);
}

ResolvedVariable resolveVariable(const Circuit& circuit,
                                 const PrintVariable& variable){
    ResolvedVariable resolved;
    resolved.quantity = variable.quantity;

    if(variable.quantity == PrintQuantity::Voltage){
        resolved.index = SpiceOutputAccess::nodeMap(circuit).idxOf(variable.name);
        resolved.referenceIndex =
            SpiceOutputAccess::nodeMap(circuit).idxOf(variable.reference);
        resolved.label = "v(" + to_lower_copy(variable.name);
        if(variable.reference != "0" &&
           !equal_ignore_case(variable.reference, "gnd")){
            resolved.label += ',' + to_lower_copy(variable.reference);
        }
        resolved.label += ')';
        resolved.rawLabel = resolved.label;
        resolved.rawType = "voltage";
        return resolved;
    }

    resolved.index = branchIndexOf(circuit, variable.name);
    resolved.label = to_lower_copy(variable.name) + "#branch";
    resolved.rawLabel = "i(" + to_lower_copy(variable.name) + ')';
    resolved.rawType = "current";
    return resolved;
}

std::vector<ResolvedVariable> defaultVariables(const Circuit& circuit){
    std::vector<ResolvedVariable> resolved;

    const auto& nodeNames =
        SpiceOutputAccess::nodeMap(circuit).nodeNameByIdx();
    resolved.reserve(
        nodeNames.size() + SpiceOutputAccess::devices(circuit).size()
    );
    for(std::size_t i = 0; i < nodeNames.size(); ++i){
        resolved.push_back({
            PrintQuantity::Voltage,
            "v(" + to_lower_copy(nodeNames[i]) + ')',
            "v(" + to_lower_copy(nodeNames[i]) + ')',
            "voltage",
            static_cast<int>(i),
            -1
        });
    }

    for(const auto& device: SpiceOutputAccess::devices(circuit)){
        const int branch = device->branchUnknown();
        if(branch >= 0){
            resolved.push_back({
                PrintQuantity::BranchCurrent,
                to_lower_copy(device->getName()) + "#branch",
                "i(" + to_lower_copy(device->getName()) + ')',
                "current",
                branch,
                -1
            });
        }
    }
    return resolved;
}

std::vector<ResolvedVariable> resolveVariables(
    const Circuit& circuit,
    bool printRequested,
    const std::vector<PrintVariable>& requested)
{
    if(!printRequested){
        return defaultVariables(circuit);
    }

    std::vector<ResolvedVariable> resolved;
    resolved.reserve(requested.size());
    for(const auto& variable: requested){
        resolved.push_back(resolveVariable(circuit, variable));
    }
    return resolved;
}

double variableValue(const ResolvedVariable& variable,
                     const Eigen::VectorXd& solution){
    if(variable.quantity == PrintQuantity::BranchCurrent){
        return solution[variable.index];
    }

    const double positive = variable.index < 0
        ? 0.0
        : solution[variable.index];
    const double negative = variable.referenceIndex < 0
        ? 0.0
        : solution[variable.referenceIndex];
    return positive - negative;
}

void ensureFinite(double value){
    if(!std::isfinite(value)){
        throw std::runtime_error("Cannot write NaN or infinity to SPICE output");
    }
}

std::string currentSpiceDate(){
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif

    std::ostringstream formatted;
    formatted.imbue(std::locale::classic());
    formatted << std::put_time(&localTime, "%a %b %d %H:%M:%S %Y");
    return formatted.str();
}

void writeTableHeader(std::ostream& os,
                      const std::string& title,
                      const std::string& analysisName,
                      std::size_t pointCount,
                      bool includeTime,
                      const std::vector<ResolvedVariable>& variables){
    const std::size_t columnCount = variables.size() + (includeTime ? 1 : 0);
    const std::size_t separatorWidth = std::max<std::size_t>(
        80,
        kIndexWidth + kValueWidth * columnCount
    );

    os << "Circuit: " << title << "\n\n";
    os << analysisName << '\n';
    os << "No. of Data Rows : " << pointCount << "\n\n";
    os << std::string(separatorWidth, '-') << '\n';
    os << std::left;
    if(columnCount == 0){
        os << "Index";
    } else {
        os << std::setw(kIndexWidth) << "Index";
    }
    if(includeTime){
        os << ' ';
        if(variables.empty()){
            os << "time";
        } else {
            os << std::setw(kValueWidth - 1) << "time";
        }
    }
    for(std::size_t i = 0; i < variables.size(); ++i){
        os << ' ';
        if(i + 1 == variables.size()){
            os << variables[i].label;
        } else {
            os << std::setw(kValueWidth - 1) << variables[i].label;
        }
    }
    os << '\n' << std::string(separatorWidth, '-') << '\n';
}

void writeTableRow(std::ostream& os,
                   std::size_t index,
                   const double* time,
                   const Eigen::VectorXd& solution,
                   const std::vector<ResolvedVariable>& variables){
    os << std::right << std::setw(kIndexWidth) << index;
    os << std::scientific << std::setprecision(10);
    if(time){
        ensureFinite(*time);
        os << ' ' << std::setw(kValueWidth - 1) << *time;
    }
    for(const auto& variable: variables){
        const double value = variableValue(variable, solution);
        ensureFinite(value);
        os << ' ' << std::setw(kValueWidth - 1) << value;
    }
    os << '\n';
}


void writeRawHeader(std::ostream& os,
                    const std::string& title,
                    const std::string& plotName,
                    std::size_t pointCount,
                    bool includeTime,
                    const std::vector<ResolvedVariable>& variables){
    const std::size_t variableCount =
        variables.size() + (includeTime ? 1 : 0);

    os << "Title: " << title << '\n';
    os << "Date: " << currentSpiceDate() << '\n';
    os << "Plotname: " << plotName << '\n';
    os << "Flags: real\n";
    os << "No. Variables: " << variableCount << '\n';
    os << "No. Points: " << pointCount << '\n';
    os << "Variables:\n";

    std::size_t variableIndex = 0;
    if(includeTime){
        os << '\t' << variableIndex++ << "\ttime\ttime\n";
    }
    for(const auto& variable: variables){
        os << '\t' << variableIndex++ << '\t'
           << variable.rawLabel << '\t' << variable.rawType << '\n';
    }
    os << "Values:\n";
}

void writeRawPoint(std::ostream& os,
                   std::size_t pointIndex,
                   const double* time,
                   const Eigen::VectorXd& solution,
                   const std::vector<ResolvedVariable>& variables){
    os << ' ' << pointIndex;
    std::size_t firstVariable = 0;

    if(time){
        ensureFinite(*time);
        os << '\t' << *time << '\n';
    } else if(!variables.empty()){
        const double value = variableValue(variables.front(), solution);
        ensureFinite(value);
        os << '\t' << value << '\n';
        firstVariable = 1;
    } else {
        os << '\n';
    }

    for(std::size_t i = firstVariable; i < variables.size(); ++i){
        const double value = variableValue(variables[i], solution);
        ensureFinite(value);
        os << '\t' << value << '\n';
    }
    os << '\n';
}
}

void SpiceOutputWriter::writeOperatingPoint(std::ostream& os,
                                             const Circuit& circuit,
                                             const std::string& title,
                                             const AnalysisPlan& plan){
    os.imbue(std::locale::classic());
    const auto variables = resolveVariables(
        circuit,
        plan.operatingPointPrintRequested,
        plan.operatingPointPrints
    );
    writeTableHeader(
        os,
        title,
        "Operating Point",
        1,
        false,
        variables
    );
    writeTableRow(
        os,
        0,
        nullptr,
        SpiceOutputAccess::mna(circuit).solution(),
        variables
    );
}

void SpiceOutputWriter::writeTransient(std::ostream& os,
                                        const Circuit& circuit,
                                        const std::string& title,
                                        const AnalysisPlan& plan){
    os.imbue(std::locale::classic());
    const auto variables = resolveVariables(
        circuit,
        plan.transientPrintRequested,
        plan.transientPrints
    );
    writeTableHeader(
        os,
        title,
        "Transient Analysis",
        SpiceOutputAccess::transientSamples(circuit).size(),
        true,
        variables
    );

    const auto& samples = SpiceOutputAccess::transientSamples(circuit);
    for(std::size_t i = 0; i < samples.size(); ++i){
        const auto& sample = samples[i];
        writeTableRow(os, i, &sample.time, sample.solution, variables);
    }
}

void SpiceOutputWriter::writePtaDiagnostics(std::ostream& os,
                                             const Circuit& circuit){
    os.imbue(std::locale::classic());

    const PtaDiagnostics diagnostics = circuit.ptaDiagnostics();
    os << "PTA diagnostics:\n";
    if(!diagnostics.attempted){
        os << "  status: PTA was not invoked\n";
        return;
    }

    os << "  converged: " << (diagnostics.converged ? "true" : "false")
       << "\n"
       << "  iterations: " << diagnostics.iterations << "\n"
       << "  capacitance_growths: " << diagnostics.capacitanceGrowths << "\n"
       << "  capacitance_reductions: " << diagnostics.capacitanceReductions << "\n"
       << "  minimum_step_recoveries: "
       << diagnostics.minimumStepRecoveries << "\n";

    if(diagnostics.hasConvergenceMetrics){
        os << std::scientific << std::setprecision(10)
           << "  normalized_derivative: "
           << diagnostics.normalizedDerivative << "\n"
           << "  normalized_dc_residual: "
           << diagnostics.normalizedDcResidual << "\n";
    }

    os << "  attempt_trace:\n";
    for(const PtaStepAttemptDiagnostics& attempt: diagnostics.attempts){
        os << std::scientific << std::setprecision(10)
           << "    #" << attempt.attempt
           << " pseudo_time=[" << attempt.startTime << ", "
           << attempt.targetTime << ']'
           << " step=" << attempt.timeStep
           << " order=" << ptaIntegrationMethodName(attempt.integrationOrder)
           << " source_scale=" << attempt.sourceScale
           << " newton="
           << (attempt.newton.converged ? "converged" : "failed")
           << " newton_iterations=" << attempt.newton.iterations
           << " damped_steps=" << attempt.newton.dampedSteps
           << " backtracking_steps=" << attempt.newton.backtrackingSteps
           << " line_search_evaluations="
           << attempt.newton.lineSearchEvaluations
           << " non_monotone_step_fallbacks="
           << attempt.newton.nonMonotoneStepFallbacks
           << " trust_region_retries_exhausted="
           << (attempt.newton.trustRegionRetriesExhausted ? "yes" : "no")
           << " trust_region_exhaustion_recovery="
           << (attempt.newton.usedTrustRegionExhaustionRecovery ? "yes" : "no")
           << " trust_region_exhaustion_recovery_iterations="
           << attempt.newton.trustRegionExhaustionRecoveryIterations
           << " trust_region_trials=" << attempt.newton.trustRegionTrials
           << " trust_region_rejected_steps="
           << attempt.newton.trustRegionRejectedSteps
           << " trust_region_radius="
           << attempt.newton.finalTrustRegionRadius
           << " newton_normalized_update=";
        if(attempt.newton.hasNormalizedUpdate){
            os << attempt.newton.normalizedUpdate;
        } else {
            os << "n/a";
        }
        os << " newton_normalized_residual=";
        if(attempt.newton.hasNormalizedResidual){
            os << attempt.newton.normalizedResidual;
        } else {
            os << "n/a";
        }
        if(attempt.hasConvergenceMetrics){
            os << " normalized_derivative="
               << attempt.normalizedDerivative
               << " normalized_dc_residual="
               << attempt.normalizedDcResidual;
        } else {
            os << " normalized_derivative=n/a"
               << " normalized_dc_residual=n/a";
        }
        if(!attempt.newton.trustRegionExhaustionReason.empty()){
            os << " trust_region_exhaustion_reason=\""
               << attempt.newton.trustRegionExhaustionReason << '"';
        }
        os << " decision=" << ptaAttemptDecision(attempt);
        if(attempt.retryTimeStep > 0.0){
            os << " retry_step=" << attempt.retryTimeStep;
        }
        if(attempt.capacitanceGrowths > 0){
            os << " capacitance_growths=" << attempt.capacitanceGrowths
               << " growth_reason=minimum-step-newton-failure"
               << " growth_nodes=[";
            for(std::size_t i = 0;
                i < attempt.capacitanceGrowthNodes.size();
                ++i){
                const auto& growth = attempt.capacitanceGrowthNodes[i];
                if(i > 0){
                    os << ',';
                }
                os << (growth.nodeName.empty()
                       ? std::to_string(growth.nodeIndex)
                       : growth.nodeName)
                   << "(residual=" << growth.residualMagnitude
                   << ",capacitance=" << growth.capacitanceBefore
                   << "->" << growth.capacitanceAfter << ')';
            }
            os << ']';
        }
        if(attempt.capacitanceReductions > 0){
            os << " capacitance_reductions="
               << attempt.capacitanceReductions
               << " reduction_reason=node-voltage-sign-reversal"
               << " reduction_severity=(small="
               << attempt.smallOscillationCapacitanceReductions
               << ",medium="
               << attempt.mediumOscillationCapacitanceReductions
               << ",heavy="
               << attempt.heavyOscillationCapacitanceReductions << ')';
        }
        if(!attempt.status.empty()){
            os << " status=\"" << attempt.status << '\"';
        }
        if(!attempt.failureReason.empty()){
            os << " reason=\"" << attempt.failureReason << '\"';
        }
        os << '\n';
    }
}

void SpiceRawWriter::writeOperatingPoint(std::ostream& os,
                                          const Circuit& circuit,
                                          const std::string& title){
    os.imbue(std::locale::classic());
    os << std::scientific << std::setprecision(15);

    const auto variables = defaultVariables(circuit);
    writeRawHeader(os, title, "Operating Point", 1, false, variables);
    writeRawPoint(
        os,
        0,
        nullptr,
        SpiceOutputAccess::mna(circuit).solution(),
        variables
    );
}

void SpiceRawWriter::writeTransient(std::ostream& os,
                                     const Circuit& circuit,
                                     const std::string& title){
    os.imbue(std::locale::classic());
    os << std::scientific << std::setprecision(15);

    const auto variables = defaultVariables(circuit);
    const auto& samples = SpiceOutputAccess::transientSamples(circuit);
    writeRawHeader(
        os,
        title,
        "Transient Analysis",
        samples.size(),
        true,
        variables
    );
    for(std::size_t i = 0; i < samples.size(); ++i){
        writeRawPoint(
            os,
            i,
            &samples[i].time,
            samples[i].solution,
            variables
        );
    }
}
