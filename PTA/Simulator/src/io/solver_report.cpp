#include "io/solver_report.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <ostream>
#include <string>

#include "analysis/analysis_plan.hpp"
#include "analysis/pta_analysis.hpp"
#include "analysis/solver_options.hpp"
#include "circuit/circuit.hpp"

namespace {

const char* yesNo(bool value){
    return value ? "yes" : "no";
}

const char* succeededFailed(bool value){
    return value ? "succeeded" : "failed";
}

const char* ptaModeName(PtaMode mode){
    switch(mode){
        case PtaMode::Disabled: return "disabled";
        case PtaMode::Force: return "force";
        case PtaMode::Fallback: return "fallback";
    }
    return "unknown";
}

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

std::string operatingPointMethodPath(
    const OperatingPointDiagnostics& diagnostics
){
    if(diagnostics.finalMethod == "linear"){
        return std::string("direct sparse linear solve ") +
            succeededFailed(diagnostics.converged);
    }

    std::string path;
    if(diagnostics.directNewton.attempted){
        path = std::string("direct Newton ") +
            succeededFailed(diagnostics.directNewton.converged);
    }
    if(!diagnostics.sourceAttempts.empty()){
        if(!path.empty()){
            path += " -> ";
        }
        const bool sourceSucceeded =
            diagnostics.converged && diagnostics.finalMethod == "source stepping";
        path += std::string("source stepping ") +
            succeededFailed(sourceSucceeded);
    }
    if(diagnostics.ptaAttempted){
        if(!path.empty()){
            path += " -> ";
        }
        path += std::string("adaptive PTA ") +
            succeededFailed(diagnostics.converged);
    }
    return path.empty() ? "no completed solver method" : path;
}

int finalOperatingPointAttemptIterations(
    const OperatingPointDiagnostics& diagnostics
){
    if(!diagnostics.ptaAttempts.empty()){
        return diagnostics.ptaAttempts.back().newton.iterations;
    }
    if(!diagnostics.sourceAttempts.empty()){
        return diagnostics.sourceAttempts.back().newton.iterations;
    }
    return diagnostics.directNewton.iterations;
}

void writeNewton(std::ostream& output,
                 const std::string& indent,
                 const NewtonSolveDiagnostics& diagnostics){
    if(!diagnostics.attempted){
        output << indent << "Attempted: no\n";
        return;
    }

    output << indent << "Result: "
           << succeededFailed(diagnostics.converged) << '\n'
           << indent << "Iterations: " << diagnostics.iterations
           << " / " << diagnostics.maximumIterations << '\n';
    if(diagnostics.usedNewtonRaphson){
        output << indent << "Damped updates: " << diagnostics.dampedSteps << '\n'
               << indent << "Residual backtracking reductions: "
               << diagnostics.backtrackingSteps << '\n'
               << indent << "Line-search residual evaluations: "
               << diagnostics.lineSearchEvaluations << '\n'
               << indent << "Non-monotone line-search fallbacks: "
               << diagnostics.nonMonotoneStepFallbacks << '\n'
               << indent << "Trust-region retries exhausted: "
               << (diagnostics.trustRegionRetriesExhausted ? "yes" : "no")
               << '\n'
               << indent << "PTA trust-region exhaustion recovery: "
               << (diagnostics.usedTrustRegionExhaustionRecovery ? "yes" : "no")
               << " (iterations "
               << diagnostics.trustRegionExhaustionRecoveryIterations << ")\n"
               << indent << "Trust-region trials: "
               << diagnostics.trustRegionTrials << '\n'
               << indent << "Trust-region rejected trials: "
               << diagnostics.trustRegionRejectedSteps << '\n'
               << indent << "Trust-region radius reductions/expansions: "
               << diagnostics.trustRegionRadiusReductions << " / "
               << diagnostics.trustRegionRadiusExpansions << '\n'
               << indent << "Initial/final trust-region radius: "
               << diagnostics.initialTrustRegionRadius << " / "
               << diagnostics.finalTrustRegionRadius << '\n'
               << indent << "Final update infinity norm: "
               << diagnostics.finalDelta << '\n'
               << indent << "Final Newton step scale: "
               << diagnostics.finalStepScale << '\n'
               << indent << "Legacy absolute tolerance: "
               << diagnostics.tolerance << '\n';
        if(diagnostics.hasNormalizedUpdate){
            output << indent << "Final normalized update: "
                   << diagnostics.normalizedUpdate << '\n';
        }
        if(diagnostics.hasNormalizedResidual){
            output << indent << "Final normalized residual: "
                   << diagnostics.normalizedResidual << '\n';
        }
        if(diagnostics.hasTrustRegionRatio){
            output << indent << "Last trust-region agreement ratio: "
                   << diagnostics.lastTrustRegionRatio << '\n';
        }
        if(!diagnostics.trustRegionExhaustionReason.empty()){
            output << indent << "Trust-region exhaustion reason: "
                   << diagnostics.trustRegionExhaustionReason << '\n';
        }
    }
    output << indent << "CPU time: " << diagnostics.cpuSeconds << " s\n"
           << indent << "Wall time: " << diagnostics.wallSeconds << " s\n";
    if(!diagnostics.failureReason.empty()){
        output << indent << "Failure reason: "
               << diagnostics.failureReason << '\n';
    }
}

void writeOperatingPointAttempt(
    std::ostream& output,
    std::size_t index,
    const OperatingPointAttemptReport& attempt
){
    const auto& diagnostics = attempt.diagnostics;
    output << "\nOperating-point attempt " << index + 1 << ": "
           << attempt.label << '\n'
           << "  Result: " << succeededFailed(diagnostics.converged) << '\n'
           << "  Method path: "
           << operatingPointMethodPath(diagnostics) << '\n'
           << "  Total iterations: " << diagnostics.iterations << '\n'
           << "  Final solver attempt iterations: "
           << finalOperatingPointAttemptIterations(diagnostics) << '\n';
    if(diagnostics.finalMethod != "linear"){
        output << "  Total damped updates: " << diagnostics.dampedSteps << '\n'
               << "  Final update infinity norm: "
               << diagnostics.finalDelta << '\n';
    }
    output << "  CPU time: " << diagnostics.cpuSeconds << " s\n"
           << "  Wall time: " << diagnostics.wallSeconds << " s\n";
    if(!diagnostics.failureReason.empty()){
        output << "  Failure reason: " << diagnostics.failureReason << '\n';
    }

    if(diagnostics.directNewton.attempted){
        output << "\n  "
               << (diagnostics.finalMethod == "linear"
                   ? "Direct sparse linear solve"
                   : "Direct Newton-Raphson")
               << ":\n";
        writeNewton(output, "    ", diagnostics.directNewton);
    }

    if(!diagnostics.sourceAttempts.empty()){
        output << "\n  Source stepping:\n"
               << "    Accepted steps: " << diagnostics.sourceSteps << '\n'
               << "    Rejected steps: "
               << diagnostics.failedSourceSteps << '\n'
               << "    Final source scale: "
               << diagnostics.sourceScale << '\n'
               << "    Configured minimum source step: "
               << diagnostics.minSourceStep << '\n'
               << "    Attempt trace:\n";
        for(const auto& source: diagnostics.sourceAttempts){
            output << "      #" << source.attempt
                   << " accepted_scale=" << source.acceptedScaleBefore
                   << " step=" << source.stepSize
                   << " target_scale=" << source.targetScale
                   << " result=" << (source.accepted ? "accepted" : "rejected")
                   << " iterations=" << source.newton.iterations
                   << " damped=" << source.newton.dampedSteps
                   << " final_delta=" << source.newton.finalDelta;
            if(source.newton.hasNormalizedUpdate){
                output << " normalized_update="
                       << source.newton.normalizedUpdate;
            }
            if(source.newton.hasNormalizedResidual){
                output << " normalized_residual="
                       << source.newton.normalizedResidual;
            }
            if(!source.status.empty()){
                output << " status=\"" << source.status << '"';
            }
            if(!source.newton.failureReason.empty()){
                output << " reason=\"" << source.newton.failureReason << '"';
            }
            output << '\n';
        }
    }

    if(diagnostics.ptaAttempted){
        output << "\n  Adaptive pseudo-transient analysis (PTA):\n"
               << "    Result: " << succeededFailed(diagnostics.converged)
               << '\n'
               << "    Accepted pseudo-time steps: "
               << diagnostics.ptaAcceptedSteps << '\n'
               << "    Rejected pseudo-time steps: "
               << diagnostics.ptaRejectedSteps << '\n'
               << "    Newton iterations: "
               << diagnostics.ptaIterations << '\n'
               << "    Damped updates: "
               << diagnostics.ptaDampedSteps << '\n'
               << "    Final pseudo-time: "
               << diagnostics.ptaFinalTime << " s\n"
               << "    Final attempted step: "
               << diagnostics.ptaFinalStep << " s\n"
               << "    Attempted step range: ["
               << diagnostics.ptaMinimumAttemptedStep << ", "
               << diagnostics.ptaMaximumAttemptedStep << "] s\n"
               << "    Final source-ramp scale: "
               << diagnostics.ptaFinalSourceScale << '\n'
               << "    Global capacitance growths: "
               << diagnostics.ptaCapacitanceGrowths << '\n'
               << "    Node capacitance reductions: "
               << diagnostics.ptaCapacitanceReductions << '\n'
               << "    Minimum-step recoveries: "
               << diagnostics.ptaMinimumStepRecoveries << '\n'
               << "    CPU time: " << diagnostics.ptaCpuSeconds << " s\n"
               << "    Wall time: " << diagnostics.ptaWallSeconds << " s\n";
        if(diagnostics.hasPtaConvergenceMetrics){
            output << "    Final normalized derivative: "
                   << diagnostics.ptaNormalizedDerivative << '\n'
                   << "    Final normalized DC residual: "
                   << diagnostics.ptaNormalizedDcResidual << '\n';
        }

        output << "    Attempt trace:\n";
        for(const auto& pta: diagnostics.ptaAttempts){
            output << "      #" << pta.attempt
                   << " t=[" << pta.startTime << ", " << pta.targetTime << ']'
                   << " step=" << pta.timeStep
                   << " order=" << ptaIntegrationMethodName(
                       pta.integrationOrder
                   )
                   << " source_scale=" << pta.sourceScale
                   << " newton=" << succeededFailed(pta.newton.converged)
                   << " iterations=" << pta.newton.iterations
                   << " damped=" << pta.newton.dampedSteps
                   << " backtracks=" << pta.newton.backtrackingSteps
                   << " line_search_evaluations="
                   << pta.newton.lineSearchEvaluations;
            if(pta.newton.hasNormalizedUpdate){
                output << " newton_normalized_update="
                       << pta.newton.normalizedUpdate;
            } else {
                output << " newton_normalized_update=n/a";
            }
            if(pta.newton.hasNormalizedResidual){
                output << " newton_normalized_residual="
                       << pta.newton.normalizedResidual;
            } else {
                output << " newton_normalized_residual=n/a";
            }
            output
                   << " result=" << ptaAttemptDecision(pta);
            if(pta.hasConvergenceMetrics){
                output << " derivative=" << pta.normalizedDerivative
                       << " dc_residual=" << pta.normalizedDcResidual;
            } else {
                output << " derivative=n/a dc_residual=n/a";
            }
            if(pta.retryTimeStep > 0.0){
                output << " retry_step=" << pta.retryTimeStep;
            }
            if(pta.capacitanceGrowths > 0){
                output << " capacitance_growths=" << pta.capacitanceGrowths
                       << " growth_reason=minimum-step-newton-failure"
                       << " growth_nodes=[";
                for(std::size_t i = 0;
                    i < pta.capacitanceGrowthNodes.size();
                    ++i){
                    const auto& growth = pta.capacitanceGrowthNodes[i];
                    if(i > 0){
                        output << ',';
                    }
                    output << (growth.nodeName.empty()
                               ? std::to_string(growth.nodeIndex)
                               : growth.nodeName)
                           << "(residual=" << growth.residualMagnitude
                           << ",capacitance=" << growth.capacitanceBefore
                           << "->" << growth.capacitanceAfter << ')';
                }
                output << ']';
            }
            if(pta.capacitanceReductions > 0){
                output << " capacitance_reductions="
                       << pta.capacitanceReductions
                       << " reduction_reason=node-voltage-sign-reversal"
                       << " reduction_severity=(small="
                       << pta.smallOscillationCapacitanceReductions
                       << ",medium="
                       << pta.mediumOscillationCapacitanceReductions
                       << ",heavy="
                       << pta.heavyOscillationCapacitanceReductions << ')';
            }
            if(!pta.status.empty()){
                output << " status=\"" << pta.status << '"';
            }
            if(!pta.failureReason.empty()){
                output << " reason=\"" << pta.failureReason << '"';
            }
            output << '\n';
        }
    }
}

void writeTransient(std::ostream& output,
                    const TransientDiagnostics& diagnostics){
    output << "\nTransient analysis:\n"
           << "  Result: " << succeededFailed(diagnostics.converged) << '\n'
           << "  Method: " << diagnostics.finalMethod << '\n'
           << "  Initial condition mode: "
           << (diagnostics.usedInitialConditions
               ? "UIC zero-vector initialization"
               : "operating-point initialization") << '\n'
           << "  Accepted time steps: " << diagnostics.timeSteps << '\n'
           << "  Output points: " << diagnostics.outputPoints << '\n'
           << "  Linear/Newton solve attempts: "
           << diagnostics.attemptedSteps << '\n'
           << "  Backward Euler attempts: "
           << diagnostics.backwardEulerAttempts << '\n'
           << "  BDF2 attempts: " << diagnostics.bdf2Attempts << '\n'
           << "  Total Newton/linear iterations: "
           << diagnostics.iterations << '\n';
    if(diagnostics.lastNewton.usedNewtonRaphson){
        output << "  Damped Newton updates: "
               << diagnostics.dampedSteps << '\n';
    }
    output << "  Rejected steps: " << diagnostics.rejectedSteps
           << " (convergence=" << diagnostics.convergenceRejectedSteps
           << ", error=" << diagnostics.errorRejectedSteps
           << ", invalid-estimate=" << diagnostics.invalidEstimateFailures
           << ")\n"
           << "  Final simulated time: " << diagnostics.finalTime << " s\n"
           << "  Attempted step range: ["
           << diagnostics.minimumAttemptedStep << ", "
           << diagnostics.maximumAttemptedStep << "] s\n"
           << "  Accepted step range: ["
           << diagnostics.minimumAcceptedStep << ", "
           << diagnostics.maximumAcceptedStep << "] s\n";
    if(diagnostics.lastNewton.usedNewtonRaphson){
        output << "  Final Newton update infinity norm: "
               << diagnostics.finalDelta << '\n'
               << "  Newton tolerance: " << diagnostics.tolerance << '\n';
    }
    output << "  Initialization CPU/wall time: "
           << diagnostics.initializationCpuSeconds << " / "
           << diagnostics.initializationWallSeconds << " s\n"
           << "  Total CPU/wall time: " << diagnostics.cpuSeconds << " / "
           << diagnostics.wallSeconds << " s\n";
    if(diagnostics.hasNormalizedError){
        output << "  Last normalized local error: "
               << diagnostics.lastNormalizedError << '\n'
               << "  Maximum normalized local error: "
               << diagnostics.maximumNormalizedError << '\n';
    }
    if(!diagnostics.failureReason.empty()){
        output << "  Failure reason: " << diagnostics.failureReason << '\n';
    }
    if(diagnostics.lastNewton.attempted){
        output << "  Final linear/Newton attempt:\n";
        writeNewton(output, "    ", diagnostics.lastNewton);
    }
}

void writeNewtonConfiguration(std::ostream& output,
                              const std::string& prefix,
                              const NewtonSolverOptions& options){
    output << "  " << prefix << ".maximum_iterations: "
           << options.maximumIterations << '\n'
           << "  " << prefix << ".tolerance (legacy): "
           << options.tolerance << '\n'
           << "  " << prefix << ".relative_tolerance: "
           << options.relativeTolerance << '\n'
           << "  " << prefix << ".voltage_absolute_tolerance: "
           << options.voltageAbsoluteTolerance << '\n'
           << "  " << prefix << ".current_absolute_tolerance: "
           << options.currentAbsoluteTolerance << '\n'
           << "  " << prefix << ".normalized_update_tolerance: "
           << options.normalizedUpdateTolerance << '\n'
           << "  " << prefix << ".normalized_residual_tolerance: "
           << options.normalizedResidualTolerance << '\n'
           << "  " << prefix << ".maximum_backtracks: "
           << options.maximumBacktracks << '\n'
           << "  " << prefix << ".backtrack_scale: "
           << options.backtrackScale << '\n'
           << "  " << prefix << ".sufficient_decrease: "
           << options.sufficientDecrease << '\n'
           << "  " << prefix << ".maximum_solution_step: "
           << options.maximumSolutionStep << '\n'
           << "  " << prefix
           << ".maximum_consecutive_non_monotone_steps: "
           << options.maximumConsecutiveNonMonotoneSteps << '\n'
           << "  " << prefix
           << ".maximum_non_monotone_residual_growth: "
           << options.maximumNonMonotoneResidualGrowth << '\n'
           << "  " << prefix << ".trust_region_enabled: "
           << yesNo(options.trustRegionEnabled) << '\n'
           << "  " << prefix << ".trust_region_initial_radius: "
           << options.trustRegionInitialRadius << '\n'
           << "  " << prefix << ".trust_region_minimum_radius: "
           << options.trustRegionMinimumRadius << '\n'
           << "  " << prefix << ".trust_region_maximum_radius: "
           << options.trustRegionMaximumRadius << '\n'
           << "  " << prefix << ".maximum_trust_region_retries: "
           << options.maximumTrustRegionRetries << '\n'
           << "  " << prefix << ".trust_region_acceptance_ratio: "
           << options.trustRegionAcceptanceRatio << '\n'
           << "  " << prefix << ".trust_region_shrink_threshold: "
           << options.trustRegionShrinkThreshold << '\n'
           << "  " << prefix << ".trust_region_grow_threshold: "
           << options.trustRegionGrowThreshold << '\n'
           << "  " << prefix << ".trust_region_shrink_factor: "
           << options.trustRegionShrinkFactor << '\n'
           << "  " << prefix << ".trust_region_grow_factor: "
           << options.trustRegionGrowFactor << '\n'
           << "  " << prefix << ".trust_region_boundary_fraction: "
           << options.trustRegionBoundaryFraction << '\n';
}

void writeConfiguration(
    std::ostream& output,
    const AnalysisPlan* plan,
    const OperatingPointSolverOptions* operatingPoint,
    const PtaAnalysisConfig* pta
){
    output << "\nEffective solver configuration:\n";
    if(operatingPoint){
        writeNewtonConfiguration(output, "op.newton", operatingPoint->newton);
        const auto& source = operatingPoint->sourceStepping;
        output << "  op.source_stepping.enabled: " << yesNo(source.enabled) << '\n'
               << "  op.source_stepping.initial_step: " << source.initialStep << '\n'
               << "  op.source_stepping.maximum_step: " << source.maximumStep << '\n'
               << "  op.source_stepping.minimum_step: " << source.minimumStep << '\n'
               << "  op.source_stepping.growth_factor: " << source.growthFactor << '\n'
               << "  op.source_stepping.failure_scale: " << source.failureScale << '\n';
    }
    if(pta){
        writeNewtonConfiguration(output, "pta.newton", pta->newtonOptions);
        output << "  pta.mode: " << ptaModeName(pta->mode) << '\n'
               << "  pta.steps (minimum/initial/maximum): "
               << pta->minimumStep << " / " << pta->initialStep << " / "
               << pta->maximumStep << '\n'
               << "  pta.maximum_steps: " << pta->maximumSteps << '\n'
               << "  pta.derivative_tolerance: " << pta->derivativeTolerance << '\n'
               << "  pta.derivative_relative_tolerance: "
               << pta->derivativeRelativeTolerance << '\n'
               << "  pta.derivative_voltage_absolute_tolerance: "
               << pta->derivativeVoltageAbsoluteTolerance << '\n'
               << "  pta.derivative_current_absolute_tolerance: "
               << pta->derivativeCurrentAbsoluteTolerance << '\n'
               << "  pta.dc_residual_tolerance: " << pta->dcResidualTolerance << '\n'
               << "  pta.dc_residual_relative_tolerance: "
               << pta->dcResidualRelativeTolerance << '\n'
               << "  pta.dc_voltage_absolute_tolerance: "
               << pta->dcVoltageAbsoluteTolerance << '\n'
               << "  pta.dc_current_absolute_tolerance: "
               << pta->dcCurrentAbsoluteTolerance << '\n'
               << "  pta.node_capacitance (minimum/initial/maximum): "
               << pta->minimumNodeCapacitance << " / "
               << pta->initialNodeCapacitance << " / "
               << pta->maximumNodeCapacitance << '\n'
               << "  pta.current_source_capacitance: "
               << pta->currentSourceCapacitance << '\n'
               << "  pta.voltage_source_inductance: "
               << pta->voltageSourceInductance << '\n'
               << "  pta.compound_time_constant: "
               << pta->compoundTimeConstant << '\n'
               << "  pta.compound_initial_resistance: "
               << pta->compoundInitialResistance << '\n'
               << "  pta.compound_initial_conductance: "
               << pta->compoundInitialConductance << '\n'
               << "  pta.source_ramp_time: " << pta->sourceRampTime << '\n'
               << "  pta.initial_mos_vgs: ";
        if(pta->initialMosVgs){
            output << *pta->initialMosVgs << '\n';
        } else {
            output << "null\n";
        }
        output << "  pta.initial_bjt_vbe: ";
        if(pta->initialBjtVbe){
            output << *pta->initialBjtVbe << '\n';
        } else {
            output << "null\n";
        }
        output << "  pta.failed/successful_step_scale: "
               << pta->failedStepScale << " / "
               << pta->successfulStepScale << '\n'
               << "  pta.capacitance_grow_scale: "
               << pta->capacitanceGrowScale << '\n'
               << "  pta.oscillation_scale (small/medium/heavy): "
               << pta->smallOscillationScale << " / "
               << pta->mediumOscillationScale << " / "
               << pta->heavyOscillationScale << '\n'
               << "  pta.oscillation_ratio (medium/heavy): "
               << pta->mediumOscillationRatio << " / "
               << pta->heavyOscillationRatio << '\n'
               << "  pta.include_mos_bulk: " << yesNo(pta->includeMosBulk) << '\n'
               << "  pta.include_diodes: " << yesNo(pta->includeDiodes) << '\n';
    }
    if(plan && plan->transient){
        const auto& transient = *plan->transient;
        const auto& solver = transient.solverOptions;
        output << "  tran.output_interval: " << transient.outputInterval << '\n'
               << "  tran.stop_time: " << transient.stopTime << '\n'
               << "  tran.output_start_time: "
               << transient.outputStartTime << '\n'
               << "  tran.maximum_step: ";
        if(transient.maximumStep){
            output << *transient.maximumStep << '\n';
        } else {
            output << "output_interval\n";
        }
        output << "  tran.use_initial_conditions: "
               << yesNo(transient.useInitialConditions) << '\n';
        writeNewtonConfiguration(output, "tran.solver.newton", solver.newtonOptions);
        output << "  tran.solver.relative_tolerance: "
               << solver.relativeTolerance << '\n'
               << "  tran.solver.voltage_absolute_tolerance: "
               << solver.voltageAbsoluteTolerance << '\n'
               << "  tran.solver.current_absolute_tolerance: "
               << solver.currentAbsoluteTolerance << '\n'
               << "  tran.solver.minimum_step: " << solver.minimumStep << '\n'
               << "  tran.solver.safety_factor: " << solver.safetyFactor << '\n'
               << "  tran.solver.maximum_rejects: " << solver.maximumRejects << '\n'
               << "  tran.solver.step_scale (minimum/maximum): "
               << solver.minimumScale << " / " << solver.maximumScale << '\n'
               << "  tran.solver.convergence_failure_scale: "
               << solver.convergenceFailureScale << '\n';
    }
}

void writeCircuitDiagnostics(std::ostream& output,
                             const Circuit& circuit){
    const CircuitDiagnostics diagnostics = circuit.circuitDiagnostics();
    output << "\nCircuit characteristics:\n"
           << "  Primitive devices: " << diagnostics.deviceCount << '\n'
           << "  Nonlinear devices: "
           << diagnostics.nonlinearDeviceCount << '\n'
           << "  Models: " << diagnostics.modelCount << '\n'
           << "  Non-ground nodes: " << diagnostics.nodeCount << '\n'
           << "  MNA unknowns: " << diagnostics.unknownCount << '\n'
           << "  MNA structural nonzeros: "
           << diagnostics.matrixNonZeros << '\n';
    if(diagnostics.unknownCount > 0){
        const double size = static_cast<double>(diagnostics.unknownCount);
        output << "  MNA structural density: "
               << static_cast<double>(diagnostics.matrixNonZeros) /
                      (size * size) << '\n';
    }
    output << "  PTA pseudo-devices: "
           << diagnostics.pseudoDeviceCount << '\n'
           << "  Device mix:";
    if(diagnostics.devicesByType.empty()){
        output << " unavailable\n";
    } else {
        output << '\n';
        for(const auto& device: diagnostics.devicesByType){
            output << "    " << device.type << ": " << device.count << '\n';
        }
    }

    if(diagnostics.hasFiniteSolution){
        output << "  Current solution node-voltage range: ["
               << diagnostics.minimumNodeVoltage << ", "
               << diagnostics.maximumNodeVoltage << "] V\n"
               << "  Largest absolute solution component: "
               << diagnostics.maximumAbsoluteSolution << " ("
               << (diagnostics.maximumAbsoluteSolutionVariable.empty()
                   ? "unknown"
                   : diagnostics.maximumAbsoluteSolutionVariable)
               << ")\n";
        if(!diagnostics.maximumAbsoluteBranchCurrentDevice.empty()){
            output << "  Largest absolute branch current: "
                   << diagnostics.maximumAbsoluteBranchCurrent << " A ("
                   << diagnostics.maximumAbsoluteBranchCurrentDevice << ")\n";
        }
    } else {
        output << "  Current solution vector: unavailable or non-finite\n";
    }
}

void writeRecommendations(std::ostream& output,
                          const SimulationReport& report,
                          const Circuit* circuit){
    output << "\nConvergence and tuning observations:\n";
    bool wroteObservation = false;

    for(const auto& attempt: report.operatingPointAttempts){
        const auto& diagnostics = attempt.diagnostics;
        if(diagnostics.directNewton.attempted &&
           !diagnostics.directNewton.converged && diagnostics.converged){
            output << "  - Direct Newton did not converge, but "
                   << diagnostics.finalMethod
                   << " recovered the operating point. Preserve the recovery "
                      "method while changing tolerances or model parameters.\n";
            wroteObservation = true;
        }
        if(!diagnostics.converged){
            output << "  - " << attempt.label << " failed";
            if(!diagnostics.failureReason.empty()){
                output << " because " << diagnostics.failureReason;
            }
            output << ". Inspect floating nodes, conflicting ideal sources, "
                      "device polarity, and unrealistic model/value scales first.\n";
            wroteObservation = true;
        }
        if(diagnostics.directNewton.attempted &&
           diagnostics.finalMethod != "linear" &&
           diagnostics.directNewton.maximumIterations > 0 &&
           diagnostics.directNewton.iterations >=
               diagnostics.directNewton.maximumIterations){
            output << "  - Direct Newton exhausted its iteration budget. "
                      "Consider a smaller maximum solution step, then a larger "
                      "iteration limit if the update norm is still decreasing.\n";
            wroteObservation = true;
        }
        if(diagnostics.failedSourceSteps > 0){
            output << "  - Source stepping rejected "
                   << diagnostics.failedSourceSteps
                   << " trial(s). A smaller initial/maximum source step or a "
                      "smaller minimum step can make continuation more gradual.\n";
            wroteObservation = true;
        }
        if(diagnostics.ptaAttempted && diagnostics.ptaRejectedSteps > 0){
            output << "  - PTA rejected " << diagnostics.ptaRejectedSteps
                   << " pseudo-time step(s). Consider a smaller PTA initial step "
                      "or failed-step scale before relaxing convergence tolerances.\n";
            wroteObservation = true;
        }
        if(diagnostics.ptaCapacitanceGrowths > 0){
            output << "  - PTA required artificial-capacitance growth "
                   << diagnostics.ptaCapacitanceGrowths
                   << " time(s), indicating a stiff or oscillatory startup. "
                      "Review fast feedback loops and initial bias assumptions.\n";
            wroteObservation = true;
        }
    }

    if(report.transient){
        const auto& transient = *report.transient;
        if(transient.convergenceRejectedSteps > 0){
            output << "  - Transient Newton convergence rejected "
                   << transient.convergenceRejectedSteps
                   << " step(s). Reduce TMAX/maximum_step or increase the "
                      "transient Newton iteration budget.\n";
            wroteObservation = true;
        }
        if(transient.errorRejectedSteps > 0){
            output << "  - Local-error control rejected "
                   << transient.errorRejectedSteps
                   << " step(s). A smaller maximum step is safer than relaxing "
                      "relative/absolute error tolerances without validation.\n";
            wroteObservation = true;
        }
        if(!transient.converged){
            output << "  - Transient analysis stopped at t="
                   << transient.finalTime << " s";
            if(!transient.failureReason.empty()){
                output << " because " << transient.failureReason;
            }
            output << ". Inspect the final attempted time/step and the last "
                      "Newton diagnostic above.\n";
            wroteObservation = true;
        }
    }

    if(circuit){
        const CircuitDiagnostics diagnostics = circuit->circuitDiagnostics();
        if(diagnostics.hasFiniteSolution &&
           (std::abs(diagnostics.minimumNodeVoltage) > 1.0e6 ||
            std::abs(diagnostics.maximumNodeVoltage) > 1.0e6)){
            output << "  - The solution contains megavolt-scale node values. "
                      "Check units, floating high-impedance nodes, and source/model "
                      "magnitudes before treating convergence as physical.\n";
            wroteObservation = true;
        }
        if(diagnostics.hasFiniteSolution &&
           diagnostics.maximumAbsoluteBranchCurrent > 1.0e3){
            output << "  - The solution contains kiloamp-scale branch current. "
                      "Check ideal-source conflicts, very small impedances, and units.\n";
            wroteObservation = true;
        }
    }

    if(!wroteObservation){
        output << "  - No convergence warning was detected from the recorded "
                  "solver metrics.\n";
    }
    output << "  - Change one numerical parameter at a time and confirm that "
              "voltages, currents, conservation laws, and expected operating "
              "regions remain physically plausible.\n";
}

}  // namespace

void SolverReportWriter::write(
    std::ostream& output,
    const SimulationReport& report,
    const Circuit* circuit,
    const AnalysisPlan* plan,
    const OperatingPointSolverOptions* operatingPointConfig,
    const PtaAnalysisConfig* ptaConfig
){
    output.imbue(std::locale::classic());
    output << std::scientific << std::setprecision(10);

    output << "SPICE Solver Report\n"
           << "===================\n"
           << "Input netlist: " << report.inputPath << '\n'
           << "Circuit title: "
           << (report.title.empty() ? "unavailable" : report.title) << '\n'
           << "Configuration source: "
           << (report.configurationSource.empty()
               ? "built-in defaults"
               : report.configurationSource) << '\n'
           << "Status: " << report.status << '\n';
    if(!report.statusDetail.empty()){
        output << "Status detail: " << report.statusDetail << '\n';
    }
    output << "Total wall time: " << report.totalWallSeconds << " s\n";

    if(plan){
        output << "\nRequested analyses:\n"
               << "  Operating point: "
               << yesNo(plan->operatingPointRequested || !plan->transient)
               << '\n'
               << "  Transient: " << yesNo(plan->transient.has_value()) << '\n'
               << "  Pseudo-transient control card: "
               << yesNo(plan->pseudoTransient.has_value()) << '\n';
    }

    if(circuit){
        writeCircuitDiagnostics(output, *circuit);
    }

    output << "\nSolver method history:";
    if(report.operatingPointAttempts.empty() && !report.transient){
        output << " no solve was started\n";
    } else {
        output << '\n';
    }
    for(std::size_t i = 0; i < report.operatingPointAttempts.size(); ++i){
        writeOperatingPointAttempt(output, i, report.operatingPointAttempts[i]);
    }
    if(report.transient){
        writeTransient(output, *report.transient);
    }

    writeConfiguration(output, plan, operatingPointConfig, ptaConfig);
    writeRecommendations(output, report, circuit);
}
