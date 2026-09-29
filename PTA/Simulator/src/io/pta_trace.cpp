#include "io/pta_trace.hpp"

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <string>

#include "analysis/pta_analysis.hpp"
#include "circuit/circuit.hpp"
#include "circuit/node_map.hpp"
#include "devices/device.hpp"
#include "solver/mna.hpp"

namespace {

void writeJsonString(std::ostream& output, std::string_view value){
    output << '"';
    for(const unsigned char character: value){
        switch(character){
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if(character < 0x20){
                    output << "\\u00" << std::hex << std::setw(2)
                           << std::setfill('0') << static_cast<int>(character)
                           << std::dec << std::setfill(' ');
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    output << '"';
}

void writeJsonNumber(std::ostream& output, double value){
    if(std::isfinite(value)){
        output << value;
    } else {
        output << "null";
    }
}

const char* modeName(PtaMode mode){
    switch(mode){
        case PtaMode::Disabled: return "disabled";
        case PtaMode::Force: return "force";
        case PtaMode::Fallback: return "fallback";
    }
    return "unknown";
}

const char* integrationName(int order){
    switch(order){
        case 1: return "be";
        case 2: return "bdf2";
        default: return "unknown";
    }
}

std::uint64_t fnv1a(std::string_view input){
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for(const unsigned char value: input){
        hash ^= value;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::string configurationObject(const PtaAnalysisConfig& config){
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::scientific << std::setprecision(17);
    output << '{'
           << "\"mode\":\"" << modeName(config.mode) << "\","
           << "\"initial_step\":" << config.initialStep << ','
           << "\"minimum_step\":" << config.minimumStep << ','
           << "\"maximum_step\":" << config.maximumStep << ','
           << "\"maximum_steps\":" << config.maximumSteps << ','
           << "\"derivative_tolerance\":" << config.derivativeTolerance << ','
           << "\"derivative_relative_tolerance\":"
           << config.derivativeRelativeTolerance << ','
           << "\"derivative_voltage_absolute_tolerance\":"
           << config.derivativeVoltageAbsoluteTolerance << ','
           << "\"derivative_current_absolute_tolerance\":"
           << config.derivativeCurrentAbsoluteTolerance << ','
           << "\"dc_residual_tolerance\":" << config.dcResidualTolerance << ','
           << "\"dc_residual_relative_tolerance\":"
           << config.dcResidualRelativeTolerance << ','
           << "\"dc_voltage_absolute_tolerance\":"
           << config.dcVoltageAbsoluteTolerance << ','
           << "\"dc_current_absolute_tolerance\":"
           << config.dcCurrentAbsoluteTolerance << ','
           << "\"initial_node_capacitance\":"
           << config.initialNodeCapacitance << ','
           << "\"minimum_node_capacitance\":"
           << config.minimumNodeCapacitance << ','
           << "\"maximum_node_capacitance\":"
           << config.maximumNodeCapacitance << ','
           << "\"current_source_capacitance\":"
           << config.currentSourceCapacitance << ','
           << "\"voltage_source_inductance\":"
           << config.voltageSourceInductance << ','
           << "\"capacitance_grow_scale\":"
           << config.capacitanceGrowScale << ','
           << "\"failed_step_scale\":" << config.failedStepScale << ','
           << "\"successful_step_scale\":" << config.successfulStepScale << ','
           << "\"small_oscillation_scale\":"
           << config.smallOscillationScale << ','
           << "\"medium_oscillation_scale\":"
           << config.mediumOscillationScale << ','
           << "\"heavy_oscillation_scale\":"
           << config.heavyOscillationScale << ','
           << "\"medium_oscillation_ratio\":"
           << config.mediumOscillationRatio << ','
           << "\"heavy_oscillation_ratio\":"
           << config.heavyOscillationRatio << ','
           << "\"compound_time_constant\":"
           << config.compoundTimeConstant << ','
           << "\"compound_initial_resistance\":"
           << config.compoundInitialResistance << ','
           << "\"compound_initial_conductance\":"
           << config.compoundInitialConductance << ','
           << "\"source_ramp_time\":" << config.sourceRampTime << ','
           << "\"include_mos_bulk\":"
           << (config.includeMosBulk ? "true" : "false") << ','
           << "\"include_diodes\":"
           << (config.includeDiodes ? "true" : "false") << ','
           << "\"initial_mos_vgs\":";
    if(config.initialMosVgs){
        output << *config.initialMosVgs;
    } else {
        output << "null";
    }
    output << ",\"initial_bjt_vbe\":";
    if(config.initialBjtVbe){
        output << *config.initialBjtVbe;
    } else {
        output << "null";
    }
    output << ",\"newton\":{"
           << "\"maximum_iterations\":"
           << config.newtonOptions.maximumIterations << ','
           << "\"tolerance\":" << config.newtonOptions.tolerance << ','
           << "\"relative_tolerance\":"
           << config.newtonOptions.relativeTolerance << ','
           << "\"voltage_absolute_tolerance\":"
           << config.newtonOptions.voltageAbsoluteTolerance << ','
           << "\"current_absolute_tolerance\":"
           << config.newtonOptions.currentAbsoluteTolerance << ','
           << "\"normalized_update_tolerance\":"
           << config.newtonOptions.normalizedUpdateTolerance << ','
           << "\"normalized_residual_tolerance\":"
           << config.newtonOptions.normalizedResidualTolerance << ','
           << "\"maximum_backtracks\":"
           << config.newtonOptions.maximumBacktracks << ','
           << "\"backtrack_scale\":"
           << config.newtonOptions.backtrackScale << ','
           << "\"sufficient_decrease\":"
           << config.newtonOptions.sufficientDecrease << ','
           << "\"maximum_solution_step\":"
           << config.newtonOptions.maximumSolutionStep << ','
           << "\"maximum_consecutive_non_monotone_steps\":"
           << config.newtonOptions.maximumConsecutiveNonMonotoneSteps << ','
           << "\"maximum_non_monotone_residual_growth\":"
           << config.newtonOptions.maximumNonMonotoneResidualGrowth << ','
           << "\"trust_region_enabled\":"
           << (config.newtonOptions.trustRegionEnabled ? "true" : "false")
           << ",\"trust_region_initial_radius\":"
           << config.newtonOptions.trustRegionInitialRadius << ','
           << "\"trust_region_minimum_radius\":"
           << config.newtonOptions.trustRegionMinimumRadius << ','
           << "\"trust_region_maximum_radius\":"
           << config.newtonOptions.trustRegionMaximumRadius << ','
           << "\"maximum_trust_region_retries\":"
           << config.newtonOptions.maximumTrustRegionRetries << ','
           << "\"trust_region_acceptance_ratio\":"
           << config.newtonOptions.trustRegionAcceptanceRatio << ','
           << "\"trust_region_shrink_threshold\":"
           << config.newtonOptions.trustRegionShrinkThreshold << ','
           << "\"trust_region_grow_threshold\":"
           << config.newtonOptions.trustRegionGrowThreshold << ','
           << "\"trust_region_shrink_factor\":"
           << config.newtonOptions.trustRegionShrinkFactor << ','
           << "\"trust_region_grow_factor\":"
           << config.newtonOptions.trustRegionGrowFactor << ','
           << "\"trust_region_boundary_fraction\":"
           << config.newtonOptions.trustRegionBoundaryFraction << "}}";
    return output.str();
}

void writeNewton(std::ostream& output, const NewtonSolveDiagnostics& newton){
    output << "\"converged\":"
           << (newton.converged ? "true" : "false")
           << ",\"iterations\":" << newton.iterations
           << ",\"damped_steps\":" << newton.dampedSteps
           << ",\"backtracking_steps\":" << newton.backtrackingSteps
           << ",\"line_search_evaluations\":"
           << newton.lineSearchEvaluations
           << ",\"non_monotone_step_fallbacks\":"
           << newton.nonMonotoneStepFallbacks
           << ",\"used_trust_region\":"
           << (newton.usedTrustRegion ? "true" : "false")
           << ",\"trust_region_retries_exhausted\":"
           << (newton.trustRegionRetriesExhausted ? "true" : "false")
           << ",\"trust_region_exhaustion_recovery_used\":"
           << (newton.usedTrustRegionExhaustionRecovery ? "true" : "false")
           << ",\"trust_region_exhaustion_recovery_iterations\":"
           << newton.trustRegionExhaustionRecoveryIterations
           << ",\"trust_region_exhaustion_reason\":";
    writeJsonString(output, newton.trustRegionExhaustionReason);
    output
           << ",\"trust_region_trials\":" << newton.trustRegionTrials
           << ",\"trust_region_rejected_steps\":"
           << newton.trustRegionRejectedSteps
           << ",\"trust_region_radius_reductions\":"
           << newton.trustRegionRadiusReductions
           << ",\"trust_region_radius_expansions\":"
           << newton.trustRegionRadiusExpansions
           << ",\"initial_trust_region_radius\":";
    writeJsonNumber(output, newton.initialTrustRegionRadius);
    output << ",\"final_trust_region_radius\":";
    writeJsonNumber(output, newton.finalTrustRegionRadius);
    output << ",\"trust_region_last_ratio\":";
    if(newton.hasTrustRegionRatio){
        writeJsonNumber(output, newton.lastTrustRegionRatio);
    } else {
        output << "null";
    }
    output
           << ",\"final_step_scale\":";
    writeJsonNumber(output, newton.finalStepScale);
    output << ",\"normalized_update\":";
    if(newton.hasNormalizedUpdate){
        writeJsonNumber(output, newton.normalizedUpdate);
    } else {
        output << "null";
    }
    output << ",\"normalized_residual\":";
    if(newton.hasNormalizedResidual){
        writeJsonNumber(output, newton.normalizedResidual);
    } else {
        output << "null";
    }
    output << ",\"failure_reason\":";
    writeJsonString(output, newton.failureReason);
}

void writeGrowthNodes(std::ostream& output,
                      const PtaStepAttemptDiagnostics& attempt){
    output << '[';
    for(std::size_t i = 0; i < attempt.capacitanceGrowthNodes.size(); ++i){
        const auto& growth = attempt.capacitanceGrowthNodes[i];
        if(i > 0){
            output << ',';
        }
        output << "{\"node_index\":" << growth.nodeIndex
               << ",\"node_name\":";
        writeJsonString(output, growth.nodeName);
        output << ",\"residual_magnitude\":";
        writeJsonNumber(output, growth.residualMagnitude);
        output << ",\"capacitance_before\":";
        writeJsonNumber(output, growth.capacitanceBefore);
        output << ",\"capacitance_after\":";
        writeJsonNumber(output, growth.capacitanceAfter);
        output << '}';
    }
    output << ']';
}

}  // namespace

std::string PtaTraceWriter::write(
    const Circuit& circuit,
    const PtaAnalysisConfig& config,
    std::string_view inputPath,
    std::string_view configurationSource,
    std::string_view status,
    std::string_view statusDetail
){
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::scientific << std::setprecision(17);

    const std::string configuration = configurationObject(config);
    std::ostringstream hash;
    hash << std::hex << fnv1a(configuration);
    output << "{\"schema_version\":1,\"record_type\":\"metadata\","
           << "\"input_path\":";
    writeJsonString(output, inputPath);
    output << ",\"configuration_source\":";
    writeJsonString(output, configurationSource);
    output << ",\"configuration_hash\":\"fnv1a64:" << hash.str()
           << "\",\"configuration\":" << configuration << "}\n";

    const PtaDiagnostics diagnostics = circuit.ptaDiagnostics();
    for(const PtaStepAttemptDiagnostics& attempt: diagnostics.attempts){
        output << "{\"schema_version\":1,\"record_type\":\"attempt\","
               << "\"attempt\":" << attempt.attempt
               << ",\"pseudo_time_start\":";
        writeJsonNumber(output, attempt.startTime);
        output << ",\"pseudo_time_target\":";
        writeJsonNumber(output, attempt.targetTime);
        output << ",\"time_step\":";
        writeJsonNumber(output, attempt.timeStep);
        output << ",\"integration\":\""
               << integrationName(attempt.integrationOrder) << "\","
               << "\"source_scale\":";
        writeJsonNumber(output, attempt.sourceScale);
        output << ",\"accepted\":" << (attempt.accepted ? "true" : "false")
               << ",\"reached_steady_state\":"
               << (attempt.reachedSteadyState ? "true" : "false")
               << ",\"retry_time_step\":";
        writeJsonNumber(output, attempt.retryTimeStep);
        output << ",\"newton\":{";
        writeNewton(output, attempt.newton);
        output << "},\"normalized_derivative\":";
        if(attempt.hasConvergenceMetrics){
            writeJsonNumber(output, attempt.normalizedDerivative);
        } else {
            output << "null";
        }
        output << ",\"normalized_dc_residual\":";
        if(attempt.hasConvergenceMetrics){
            writeJsonNumber(output, attempt.normalizedDcResidual);
        } else {
            output << "null";
        }
        output << ",\"capacitance_growth_nodes\":";
        writeGrowthNodes(output, attempt);
        output << ",\"status\":";
        writeJsonString(output, attempt.status);
        output << ",\"failure_reason\":";
        writeJsonString(output, attempt.failureReason);
        output << "}\n";
    }

    output << "{\"schema_version\":1,\"record_type\":\"final\","
           << "\"status\":";
    writeJsonString(output, status);
    output << ",\"status_detail\":";
    writeJsonString(output, statusDetail);
    output << ",\"converged\":"
           << (diagnostics.converged ? "true" : "false")
           << ",\"attempt_count\":" << diagnostics.attempts.size()
           << ",\"capacitance_growths\":"
           << diagnostics.capacitanceGrowths
           << ",\"capacitance_reductions\":"
           << diagnostics.capacitanceReductions
           << ",\"solution\":{\"node_voltages\":[";

    const auto& nodeNames = circuit.nodeMap_->nodeNameByIdx();
    const Eigen::VectorXd& solution = circuit.mna_->solution();
    for(std::size_t i = 0; i < nodeNames.size(); ++i){
        if(i > 0){
            output << ',';
        }
        output << "{\"name\":";
        writeJsonString(output, nodeNames[i]);
        output << ",\"value\":";
        writeJsonNumber(output, solution[static_cast<Eigen::Index>(i)]);
        output << '}';
    }
    output << "],\"branch_currents\":[";
    bool firstBranch = true;
    for(const auto& device: circuit.devices_){
        const int branch = device->branchUnknown();
        if(branch < 0 || static_cast<Eigen::Index>(branch) >= solution.size()){
            continue;
        }
        if(!firstBranch){
            output << ',';
        }
        firstBranch = false;
        output << "{\"name\":";
        writeJsonString(output, device->getName());
        output << ",\"value\":";
        writeJsonNumber(output, solution[branch]);
        output << '}';
    }
    output << "]}}\n";
    return output.str();
}
